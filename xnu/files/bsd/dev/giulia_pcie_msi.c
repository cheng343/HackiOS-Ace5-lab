/* SPDX-License-Identifier: BSD-2-Clause */
/* Original, callback-based implementation of the SM8650 PCIe0 MSI receiver.
 * Register/route facts (fixed official revisions, not copied Linux code):
 * https://github.com/torvalds/linux/blob/v6.12/arch/arm64/boot/dts/qcom/sm8650.dtsi
 *   PCIe0 interrupts msi0..7: SPI141..148. The default msi-map selects ITS;
 *   these declarations do NOT establish that local MSI is ready this boot.
 * https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/blob/66305c56af1fcd7299c906c3257e585be93d93ed/drivers/pci/controller/pci-msm-msi.c
 *   PCIE_MSI_CTRL_*: DBI+820/824 target, +828/82c/830 enable/mask/W1C.
 *   SNPS message data is vector index; QGIC data is a GIC hwirq and its
 *   0x17110040 target needs SMMU mapping. The two routes are not interchangeable.
 * https://github.com/torvalds/linux/blob/v6.12/drivers/pci/controller/dwc/pcie-designware-host.c
 *   dw_pci_setup_msi_msg / dw_pci_bottom_ack / dw_pcie_msi_host_init:
 *   matching target at host and endpoint; vector-index data; prefer <4 GiB.
 * https://github.com/torvalds/linux/blob/v6.12/include/uapi/linux/pci_regs.h
 *   PCI_MSI_FLAGS / address / data / mask layouts below.
 * No global instance, init hook, allocation, firmware load or hardware mapping.
 */
#include "giulia_pcie_msi.h"

#define MSI_ADDR_LO 0x820u
#define MSI_ADDR_HI 0x824u
#define MSI_ENABLE  0x828u
#define MSI_MASK    0x82cu
#define MSI_STATUS  0x830u
#define MSI_CAP_ID  0x05u
#define MSI_ON      0x0001u
#define MSI_MMC     0x000eu
#define MSI_MME     0x0070u
#define MSI_64BIT   0x0080u
#define MSI_PVM     0x0100u
#define ALL_BITS   UINT32_MAX

static void source_mask(struct giulia_pcie_msi *m, uint32_t mask,
    uint32_t unmask)
{
    if (m->has_mask) {
        uint32_t old = m->io.read_cfg32(m->io.cookie, m->mask_offset);
        m->io.write_cfg32(m->io.cookie, m->mask_offset, (old | mask) & ~unmask);
    }
}

static int masks_match(struct giulia_pcie_msi *m, uint32_t enabled)
{
    if (m->io.read_dbi32(m->io.cookie, MSI_MASK) != ~enabled) return 0;
    return !m->has_mask ||
        (m->io.read_cfg32(m->io.cookie, m->mask_offset) & m->valid_mask) ==
            (m->valid_mask & ~enabled);
}

int giulia_pcie_msi_stop(struct giulia_pcie_msi *m)
{
    uint16_t control;
    if (!m) return GIULIA_MSI_ARGUMENT;
    if (m->state == GIULIA_MSI_IDLE || m->state == GIULIA_MSI_STOPPED)
        return GIULIA_MSI_OK;
    if (m->state != GIULIA_MSI_PREPARED && m->state != GIULIA_MSI_ACTIVE &&
        m->state != GIULIA_MSI_FAULT) return GIULIA_MSI_STATE;
    control = m->io.read_cfg16(m->io.cookie, m->capability + 2u);
    m->io.write_cfg16(m->io.cookie, m->capability + 2u, control & ~MSI_ON);
    m->io.barrier(m->io.cookie);
    control = m->io.read_cfg16(m->io.cookie, m->capability + 2u);
    if (control & MSI_ON) {
        /* Do not remove the interception target while the source is live. */
        m->io.write_dbi32(m->io.cookie, MSI_MASK, ALL_BITS);
        m->io.barrier(m->io.cookie);
        m->enabled_mask = 0;
        m->state = GIULIA_MSI_FAULT;
        return GIULIA_MSI_READBACK;
    }
    source_mask(m, m->valid_mask, 0);
    m->io.write_dbi32(m->io.cookie, MSI_MASK, ALL_BITS);
    m->io.write_dbi32(m->io.cookie, MSI_ENABLE, 0);
    m->io.write_dbi32(m->io.cookie, MSI_STATUS, ALL_BITS);
    m->io.barrier(m->io.cookie);
    if (m->io.read_dbi32(m->io.cookie, MSI_MASK) != ALL_BITS ||
        m->io.read_dbi32(m->io.cookie, MSI_ENABLE) != 0) {
        m->enabled_mask = 0;
        m->state = GIULIA_MSI_FAULT;
        return GIULIA_MSI_READBACK;
    }
    m->enabled_mask = 0;
    m->state = GIULIA_MSI_STOPPED;
    return GIULIA_MSI_OK;
}

int giulia_pcie_msi_prepare(struct giulia_pcie_msi *m,
    const struct giulia_pcie_msi_config *c, const struct giulia_pcie_msi_io *io)
{
    uint16_t control, data_offset, mask_offset;
    unsigned order = 0, mmc, mme, group;
    if (!m || !c || !io || !io->read_dbi32 || !io->write_dbi32 ||
        !io->read_cfg16 || !io->write_cfg16 || !io->read_cfg32 ||
        !io->write_cfg32 || !io->barrier) return GIULIA_MSI_ARGUMENT;
    if (m->state != GIULIA_MSI_IDLE && m->state != GIULIA_MSI_STOPPED)
        return GIULIA_MSI_STATE;
    if ((c->prerequisites & GIULIA_MSI_REQUIRED) != GIULIA_MSI_REQUIRED)
        return GIULIA_MSI_PREREQUISITE;
    if (!c->target || c->target > UINT32_MAX - 3u || (c->target & 3u) ||
        !c->vectors || c->vectors > 32u || (c->vectors & (c->vectors - 1u)) ||
        c->capability < 0x40u || c->capability > 0xf0u || (c->capability & 3u))
        return GIULIA_MSI_ARGUMENT;
    /* Never accept the known QGIC/ITS doorbells as an owned SNPS target. */
    if ((c->target >= 0x17110000u && c->target < 0x17120000u) ||
        (c->target >= 0x17140000u && c->target < 0x17160000u))
        return GIULIA_MSI_ARGUMENT;
    if ((io->read_cfg16(io->cookie, c->capability) & 0xffu) != MSI_CAP_ID)
        return GIULIA_MSI_CAPABILITY;
    control = io->read_cfg16(io->cookie, c->capability + 2u);
    if (control & MSI_ON) return GIULIA_MSI_SOURCE_ENABLED;
    mmc = (control & MSI_MMC) >> 1;
    mme = (control & MSI_MME) >> 4;
    while ((1u << order) < c->vectors) ++order;
    if (mmc > 5u || mme > mmc || order > mmc) return GIULIA_MSI_CAPABILITY;
    data_offset = c->capability + ((control & MSI_64BIT) ? 12u : 8u);
    mask_offset = c->capability + ((control & MSI_64BIT) ? 16u : 12u);
    if (data_offset + 2u > 0x100u ||
        ((control & MSI_PVM) && mask_offset + 8u > 0x100u))
        return GIULIA_MSI_CAPABILITY;
    /* Target registers are shared by all 8 groups. Never steal a receiver
     * from a boot-time owner, even if this caller asserts ownership. */
    for (group = 0; group < 8u; ++group)
        if (io->read_dbi32(io->cookie, MSI_ENABLE + group * 12u))
            return GIULIA_MSI_RECEIVER_BUSY;

    m->io = *io;
    m->capability = c->capability;
    m->control = (control & ~(MSI_ON | MSI_MME)) | (uint16_t)(order << 4);
    m->data_offset = data_offset;
    m->mask_offset = mask_offset;
    m->has_mask = !!(control & MSI_PVM);
    m->valid_mask = c->vectors == 32u ? ALL_BITS : (1u << c->vectors) - 1u;
    m->enabled_mask = 0;
    m->state = GIULIA_MSI_PREPARED;
    /* The caller owns the entire group; start with every vector masked. */
    source_mask(m, m->valid_mask, 0);
    io->write_dbi32(io->cookie, MSI_MASK, ALL_BITS);
    io->write_dbi32(io->cookie, MSI_ENABLE, 0);
    io->write_dbi32(io->cookie, MSI_STATUS, ALL_BITS);
    io->write_dbi32(io->cookie, MSI_ADDR_LO, (uint32_t)c->target);
    io->write_dbi32(io->cookie, MSI_ADDR_HI, 0);
    io->write_cfg32(io->cookie, c->capability + 4u, (uint32_t)c->target);
    if (control & MSI_64BIT) io->write_cfg32(io->cookie, c->capability + 8u, 0);
    io->write_cfg16(io->cookie, data_offset, 0); /* group 0, aligned vector base */
    io->write_cfg16(io->cookie, c->capability + 2u, m->control);
    io->write_dbi32(io->cookie, MSI_ENABLE, m->valid_mask);
    io->barrier(io->cookie);
    if (io->read_dbi32(io->cookie, MSI_ADDR_LO) != (uint32_t)c->target ||
        io->read_dbi32(io->cookie, MSI_ADDR_HI) != 0 ||
        !masks_match(m, 0) ||
        io->read_dbi32(io->cookie, MSI_ENABLE) != m->valid_mask ||
        io->read_cfg32(io->cookie, c->capability + 4u) != (uint32_t)c->target ||
        ((control & MSI_64BIT) && io->read_cfg32(io->cookie, c->capability + 8u)) ||
        io->read_cfg16(io->cookie, data_offset) != 0 ||
        (io->read_cfg16(io->cookie, c->capability + 2u) & (MSI_ON | MSI_MME)) !=
            (m->control & MSI_MME)) {
        (void)giulia_pcie_msi_stop(m);
        return GIULIA_MSI_READBACK;
    }
    return GIULIA_MSI_OK;
}

int giulia_pcie_msi_arm(struct giulia_pcie_msi *m, uint32_t intid,
    uint32_t enable_mask)
{
    if (!m) return GIULIA_MSI_ARGUMENT;
    if (m->state != GIULIA_MSI_PREPARED) return GIULIA_MSI_STATE;
    if (intid != GIULIA_PCIE_MSI_INTID || (enable_mask & ~m->valid_mask))
        return GIULIA_MSI_ARGUMENT;
    m->enabled_mask = enable_mask;
    m->io.write_dbi32(m->io.cookie, MSI_MASK, ~enable_mask);
    source_mask(m, m->valid_mask & ~enable_mask, enable_mask);
    m->io.barrier(m->io.cookie);
    if (!masks_match(m, enable_mask)) {
        (void)giulia_pcie_msi_stop(m);
        return GIULIA_MSI_READBACK;
    }
    m->state = GIULIA_MSI_ACTIVE;
    /* Caller has attached but still masks GIC IRQ: source enabled last. */
    m->io.write_cfg16(m->io.cookie, m->capability + 2u, m->control | MSI_ON);
    m->io.barrier(m->io.cookie);
    if ((m->io.read_cfg16(m->io.cookie, m->capability + 2u) & (MSI_ON | MSI_MME)) !=
        ((m->control & MSI_MME) | MSI_ON) ||
        !masks_match(m, enable_mask)) {
        (void)giulia_pcie_msi_stop(m);
        return GIULIA_MSI_READBACK;
    }
    return GIULIA_MSI_OK;
}

int giulia_pcie_msi_mask(struct giulia_pcie_msi *m, uint32_t bits)
{
    if (!m) return GIULIA_MSI_ARGUMENT;
    if (m->state != GIULIA_MSI_ACTIVE) return GIULIA_MSI_STATE;
    if (bits & ~m->valid_mask) return GIULIA_MSI_ARGUMENT;
    source_mask(m, bits, 0);
    m->enabled_mask &= ~bits;
    m->io.write_dbi32(m->io.cookie, MSI_MASK, ~m->enabled_mask);
    m->io.barrier(m->io.cookie);
    if (!masks_match(m, m->enabled_mask)) {
        (void)giulia_pcie_msi_stop(m);
        return GIULIA_MSI_READBACK;
    }
    return GIULIA_MSI_OK;
}

int giulia_pcie_msi_unmask(struct giulia_pcie_msi *m, uint32_t bits)
{
    if (!m) return GIULIA_MSI_ARGUMENT;
    if (m->state != GIULIA_MSI_ACTIVE) return GIULIA_MSI_STATE;
    if (bits & ~m->valid_mask) return GIULIA_MSI_ARGUMENT;
    m->enabled_mask |= bits;
    m->io.write_dbi32(m->io.cookie, MSI_MASK, ~m->enabled_mask);
    source_mask(m, 0, bits);
    m->io.barrier(m->io.cookie);
    if (!masks_match(m, m->enabled_mask)) {
        (void)giulia_pcie_msi_stop(m);
        return GIULIA_MSI_READBACK;
    }
    return GIULIA_MSI_OK;
}

uint32_t giulia_pcie_msi_dispatch(struct giulia_pcie_msi *m,
    void (*deliver)(void *, unsigned), void *cookie)
{
    uint32_t pending;
    unsigned vector;
    if (!m || !deliver || m->state != GIULIA_MSI_ACTIVE) return 0;
    pending = m->io.read_dbi32(m->io.cookie, MSI_STATUS) & m->enabled_mask;
    if (!pending) return 0;
    /* Acknowledge only this snapshot: no read-modify-write of W1C status. */
    m->io.write_dbi32(m->io.cookie, MSI_STATUS, pending);
    m->io.barrier(m->io.cookie);
    for (vector = 0; vector < GIULIA_PCIE_MSI_VECTORS; ++vector)
        if (pending & (1u << vector)) deliver(cookie, vector);
    return pending;
}
