/* SPDX-License-Identifier: BSD-2-Clause */
#include "giulia_pcie_link.h"

/* Register definitions/ordering checked against Linux v6.12:
 * drivers/pci/controller/dwc/pcie-qcom.c:
 *   qcom_pcie_init_2_7_0, qcom_pcie_configure_dbi_atu_base,
 *   qcom_pcie_link_up; SM8650 -> SM8550 -> ops_1_9_0.
 * pcie-designware.c: dw_pcie_setup, dw_pcie_link_set_max_link_width,
 *   dw_pcie_prog_outbound_atu; pcie-designware-host.c: dw_pcie_setup_rc.
 *   dw_pcie_link_set_max_speed/max_link_width and uapi/linux/pci_regs.h:
 *   link capability, target speed and negotiated status fields.
 * arch/arm64/boot/dts/qcom/sm8650.dtsi: PCIe0 PA/ranges, x2 controller.
 * https://github.com/torvalds/linux/tree/v6.12/drivers/pci/controller/dwc
 * The PARF decoder profile follows the downstream OnePlus PCIe0 convention:
 * OnePlusOSS/android_kernel_oneplus_sm8650@
 * 66305c56af1fcd7299c906c3257e585be93d93ed:
 * drivers/pci/controller/pci-msm.c:msm_pcie_enable_link sets DBI base to zero;
 * pineapple-pcie.dtsi supplies the 64 MiB slave aperture. CPU mappings remain
 * at 0x60000000/0x60001000. The local iATU decoder offset is inferred from
 * that layout; this candidate profile still requires a device test. Do not
 * mix these local offsets with mainline's full-address/large-window profile.
 * This independently implemented probe deliberately omits Linux's DMA,
 * bus-master enable, interrupt enable and SID configuration. It supports only
 * this fixed PCIe0 layout, not an arbitrary PCI host or active OS handoff.
 */
#define BIT(n) (1u << (n))
#define PARF_SYS 0x000u
#define PARF_PM 0x020u
#define PARF_PHY 0x040u
#define PARF_MHI 0x174u
#define PARF_HALT 0x1a8u
#define PARF_LTSSM 0x1b0u
#define PARF_INT_MASK 0x22cu
#define PARF_DBI_LO 0x350u
#define PARF_DBI_HI 0x354u
#define PARF_SLV_LO 0x358u
#define PARF_SLV_HI 0x35cu
#define PARF_ATU_LO 0x634u
#define PARF_ATU_HI 0x638u
#define PARF_TYPE 0x1000u
#define PARF_LOCAL_DBI 0u
#define PARF_LOCAL_ATU 0x1000u
#define PARF_SLAVE_SIZE 0x04000000u
#define PCI_COMMAND 0x04u
#define PCI_STATUS 0x06u
#define PCI_CLASS 0x0au
#define PCI_HEADER 0x0eu
#define PCI_BAR0 0x10u
#define PCI_BAR1 0x14u
#define PCI_BUS 0x18u
#define PCI_MEM 0x20u
#define PCI_CAPLIST 0x34u
#define DBI_LINK 0x710u
#define DBI_DEBUG0 0x728u
#define DBI_DEBUG1 0x72cu
#define DBI_WIDTH 0x80cu
#define DBI_MSI_ENABLE 0x828u
#define DBI_RO 0x8bcu
#define DBI_VERSION 0x8f8u
#define ATU_CTRL1 0x00u
#define ATU_CTRL2 0x04u
#define ATU_BASE_LO 0x08u
#define ATU_BASE_HI 0x0cu
#define ATU_LIMIT 0x10u
#define ATU_TARGET_LO 0x14u
#define ATU_TARGET_HI 0x18u
#define ATU_LIMIT_HI 0x20u
#define ATU_ENABLE BIT(31)
#define LNKCAP 0x0cu
#define LNKCTL 0x10u
#define LNKSTA 0x12u
#define SLTCAP 0x14u
#define LNKCTL2 0x30u
#define DLLLA BIT(13)

static uint32_t r32(struct giulia_pcie_link *s, unsigned r, uint32_t o)
{ return s->io.read32(s->io.cookie, r, o); }
static uint16_t r16(struct giulia_pcie_link *s, unsigned r, uint32_t o)
{ return s->io.read16(s->io.cookie, r, o); }
static void w32(struct giulia_pcie_link *s, unsigned r, uint32_t o, uint32_t v)
{ s->io.write32(s->io.cookie, r, o, v); s->io.barrier(s->io.cookie); }
static void w16(struct giulia_pcie_link *s, unsigned r, uint32_t o, uint16_t v)
{ s->io.write16(s->io.cookie, r, o, v); s->io.barrier(s->io.cookie); }
static int put32(struct giulia_pcie_link *s, unsigned r, uint32_t o,
    uint32_t v, uint32_t mask)
{
    uint32_t got;
    w32(s, r, o, v);
    got = r32(s, r, o);
    return got != 0xffffffffu && ((got ^ v) & mask) == 0;
}
static int put16(struct giulia_pcie_link *s, unsigned r, uint32_t o, uint16_t v)
{
    uint16_t got;
    w16(s, r, o, v);
    got = r16(s, r, o);
    return got != 0xffffu && got == v;
}
static int change32(struct giulia_pcie_link *s, unsigned r, uint32_t o,
    uint32_t clear, uint32_t set, uint32_t mask)
{
    uint32_t old = r32(s, r, o);
    return old != 0xffffffffu && put32(s, r, o, (old & ~clear) | set, mask);
}
static int change16(struct giulia_pcie_link *s, unsigned r, uint32_t o,
    uint16_t clear, uint16_t set)
{
    uint16_t old = r16(s, r, o);
    return old != 0xffffu && put16(s, r, o, (uint16_t)((old & ~clear) | set));
}
static void delay(struct giulia_pcie_link *s, uint32_t us)
{ s->io.delay_us(s->io.cookie, us); }

static int link_up(struct giulia_pcie_link *s)
{
    uint32_t v = r16(s, GIULIA_LINK_DBI, s->root_pcie_cap + LNKSTA);
    s->link_status = v;
    return v != 0xffffu && (v & DLLLA) != 0;
}

static int profile_matches(struct giulia_pcie_link *s)
{
    return (s->link_status & 0x0fu) == GIULIA_PCIE_BOOT_SPEED &&
        ((s->link_status >> 4) & 0x3fu) == GIULIA_PCIE_BOOT_LANES;
}

/* Each endpoint transaction is guarded. Like Linux's map_bus check this
 * cannot prevent an asynchronous link-down SError between check and access.
 */
static int cfg_ok(struct giulia_pcie_link *s)
{ return s->config_atu_enabled && link_up(s) && profile_matches(s); }
static int cr32(struct giulia_pcie_link *s, uint32_t o, uint32_t *v)
{
    if (!cfg_ok(s)) return 0;
    *v = r32(s, GIULIA_LINK_CFG, o);
    return 1;
}
static int cw32(struct giulia_pcie_link *s, uint32_t o, uint32_t v)
{
    if (!cfg_ok(s)) return 0;
    w32(s, GIULIA_LINK_CFG, o, v);
    return 1;
}
static int cr16(struct giulia_pcie_link *s, uint32_t o, uint16_t *v)
{
    if (!cfg_ok(s)) return 0;
    *v = r16(s, GIULIA_LINK_CFG, o);
    return *v != 0xffffu;
}
static int cp16(struct giulia_pcie_link *s, uint32_t o, uint16_t v)
{
    uint16_t got;
    if (!cfg_ok(s)) return 0;
    w16(s, GIULIA_LINK_CFG, o, v);
    return cr16(s, o, &got) && got == v;
}

static int cap_read(struct giulia_pcie_link *s, unsigned region,
    uint32_t off, uint16_t *v)
{
    if (region == GIULIA_LINK_CFG) return cr16(s, off, v);
    *v = r16(s, region, off);
    return *v != 0xffffu;
}

/* Conventional capabilities only. Detect malformed/cyclic/duplicate known
 * entries and never follow an unaligned or out-of-bounds next pointer.
 */
static int capabilities(struct giulia_pcie_link *s, unsigned region,
    uint16_t *pcie, uint16_t *msi, uint16_t *msix)
{
    uint16_t v, pos;
    uint64_t seen = 0;
    *pcie = *msi = *msix = 0;
    if (!cap_read(s, region, PCI_STATUS, &v) || !(v & BIT(4)))
        return 0;
    if (!cap_read(s, region, PCI_CAPLIST, &v)) return 0;
    pos = v & 0xffu;
    for (unsigned i = 0; pos && i < 48; i++) {
        uint64_t bit;
        if (pos < 0x40u || pos > 0xfcu || (pos & 3u)) return 0;
        bit = (uint64_t)1 << ((pos - 0x40u) / 4u);
        if (seen & bit) return 0;
        seen |= bit;
        if (!cap_read(s, region, pos, &v) || v == 0xffffu) return 0;
        switch (v & 0xffu) {
        case 0x10:
            if (*pcie || pos > 0xe8u) return 0;
            *pcie = pos;
            break;
        case 0x05:
            if (*msi || pos > 0xe8u) return 0;
            *msi = pos;
            break;
        case 0x11:
            if (*msix || pos > 0xf4u) return 0;
            *msix = pos;
            break;
        default: break;
        }
        pos = v >> 8;
    }
    return pos == 0 && *pcie != 0;
}

static int save(struct giulia_pcie_link *s, unsigned r, uint32_t o,
    unsigned width)
{
    struct giulia_pcie_link_saved *v = &s->saved[s->saved_count];
    uint32_t value = width == 2 ? r16(s, r, o) : r32(s, r, o);
    if (value == (width == 2 ? 0xffffu : 0xffffffffu)) return 0;
    v->region = (uint8_t)r;
    v->width = (uint8_t)width;
    v->offset = o;
    v->value = value;
    /* Directed speed-change is a self-clearing action, not saved state. */
    if (r == GIULIA_LINK_DBI && o == DBI_WIDTH) v->value &= ~BIT(17);
    s->saved_count++;
    return 1;
}

static int snapshot_parf(struct giulia_pcie_link *s)
{
    /* These are the only registers changed before DBI/ATU admission. LTSSM
     * is checked separately and must not be written on a foreign-core reject. */
    static const uint32_t parf[] = {PARF_TYPE, PARF_PHY,
        PARF_SYS, PARF_PM, PARF_MHI, PARF_HALT, PARF_DBI_LO, PARF_DBI_HI,
        PARF_ATU_LO, PARF_ATU_HI, PARF_SLV_LO, PARF_SLV_HI};
    if (s->mask_int_owned && !save(s, GIULIA_LINK_PARF, PARF_INT_MASK, 4))
        return 0;
    for (unsigned i = 0; i < sizeof(parf) / sizeof(parf[0]); i++)
        if (!save(s, GIULIA_LINK_PARF, parf[i], 4)) return 0;
    s->parf_count = s->saved_count;
    return 1;
}

static int snapshot_core(struct giulia_pcie_link *s)
{
    static const uint32_t dbi[] = {DBI_RO, PCI_BUS, PCI_BAR0, PCI_BAR1,
        PCI_MEM, DBI_LINK, DBI_WIDTH};
    static const uint32_t atu[] = {ATU_CTRL1, ATU_CTRL2, ATU_BASE_LO,
        ATU_BASE_HI, ATU_LIMIT, ATU_TARGET_LO, ATU_TARGET_HI, ATU_LIMIT_HI};
    if (!save(s, GIULIA_LINK_PARF, PARF_LTSSM, 4)) return 0;
    /* RO enable saved first, restored last among DBI registers. */
    for (unsigned i = 0; i < sizeof(dbi) / sizeof(dbi[0]); i++)
        if (!save(s, GIULIA_LINK_DBI, dbi[i], 4)) return 0;
    if (!save(s, GIULIA_LINK_DBI, PCI_COMMAND, 2) ||
        !save(s, GIULIA_LINK_DBI, PCI_CLASS, 2) ||
        !save(s, GIULIA_LINK_DBI, s->root_pcie_cap + LNKCAP, 4) ||
        !save(s, GIULIA_LINK_DBI, s->root_pcie_cap + SLTCAP, 4) ||
        !save(s, GIULIA_LINK_DBI, s->root_pcie_cap + LNKCTL, 2) ||
        !save(s, GIULIA_LINK_DBI, s->root_pcie_cap + LNKCTL2, 2)) return 0;
    for (unsigned n = 0; n < 2; n++)
        for (unsigned i = 0; i < sizeof(atu) / sizeof(atu[0]); i++)
            if (!save(s, GIULIA_LINK_ATU, n * 0x200u + atu[i], 4)) return 0;
    return 1;
}

static int restore_saved(struct giulia_pcie_link *s, unsigned first, unsigned count)
{
    int okay = 1;
    for (unsigned i = count; i > first; i--) {
        const struct giulia_pcie_link_saved *v = &s->saved[i - 1u];
        if (v->width == 2) {
            okay &= put16(s, v->region, v->offset, (uint16_t)v->value);
        } else {
            uint32_t mask = 0xffffffffu;
            if (v->region == GIULIA_LINK_DBI && v->offset == DBI_WIDTH)
                mask &= ~BIT(17);
            if (v->region == GIULIA_LINK_DBI && v->offset == PCI_BAR0)
                mask &= ~0xfu;
            if (v->region == GIULIA_LINK_DBI && v->offset == PCI_MEM)
                mask = 0xfff0fff0u;
            okay &= put32(s, v->region, v->offset, v->value, mask);
        }
    }
    return okay;
}

/* PARF is always accessible through its dedicated peripheral aperture.
 * This phase deliberately performs no DBI/ATU access, including on failure. */
static int configure_parf(struct giulia_pcie_link *s)
{
    if (s->mask_int_owned &&
        !put32(s, GIULIA_LINK_PARF, PARF_INT_MASK, 0, 0xffffffffu))
        return 0;
    return put32(s, GIULIA_LINK_PARF, PARF_TYPE, 4, 0xffffffffu) &&
        change32(s, GIULIA_LINK_PARF, PARF_PHY, BIT(0), 0, BIT(0)) &&
        put32(s, GIULIA_LINK_PARF, PARF_DBI_LO, PARF_LOCAL_DBI, 0xffffffffu) &&
        put32(s, GIULIA_LINK_PARF, PARF_DBI_HI, 0, 0xffffffffu) &&
        put32(s, GIULIA_LINK_PARF, PARF_ATU_LO, PARF_LOCAL_ATU, 0xffffffffu) &&
        put32(s, GIULIA_LINK_PARF, PARF_ATU_HI, 0, 0xffffffffu) &&
        put32(s, GIULIA_LINK_PARF, PARF_SLV_LO, PARF_SLAVE_SIZE, 0xffffffffu) &&
        put32(s, GIULIA_LINK_PARF, PARF_SLV_HI, 0, 0xffffffffu) &&
        change32(s, GIULIA_LINK_PARF, PARF_SYS, BIT(29), 0, BIT(29)) &&
        change32(s, GIULIA_LINK_PARF, PARF_MHI, 0, BIT(4), BIT(4)) &&
        change32(s, GIULIA_LINK_PARF, PARF_PM, BIT(5), 0, BIT(5)) &&
        change32(s, GIULIA_LINK_PARF, PARF_HALT, 0, BIT(31), BIT(31));
}

static int quiesce_controller(struct giulia_pcie_link *s)
{
    int okay = 1;
    okay &= change32(s, GIULIA_LINK_PARF, PARF_LTSSM, BIT(8), 0, BIT(8));
    for (unsigned n = 0; s->atu_touched && n < 2; n++) {
        uint32_t off = n * 0x200u + ATU_CTRL2;
        okay &= change32(s, GIULIA_LINK_ATU, off, ATU_ENABLE, 0, ATU_ENABLE);
    }
    s->config_atu_enabled = 0;
    return okay;
}

static int record_failure(struct giulia_pcie_link *s, int error)
{
    s->failure_stage = s->stage;
    s->error = error;
    s->state = GIULIA_LINK_FAULT;
    return error;
}

static int fail(struct giulia_pcie_link *s, int error)
{
    /* Full core ownership has already been admitted. Early PARF-only failures
     * use reject_after_parf instead. Complete restoration waits for the board
     * owner to assert PERST# and call stop. */
    (void)quiesce_controller(s);
    return record_failure(s, error);
}

static int reject(struct giulia_pcie_link *s, int error)
{
    /* Read-only admission failure never acquires the controller. */
    s->error = error;
    s->failure_stage = s->stage;
    return error;
}

static int reject_after_parf(struct giulia_pcie_link *s, int error)
{
    s->error = error;
    s->failure_stage = s->stage;
    /* Core admission/snapshot has not completed: never touch its registers
     * during rollback, even if an enabled MSI receiver/window was observed. */
    if (!restore_saved(s, 0, s->parf_count))
        return record_failure(s, GIULIA_LINK_READBACK);
    s->parf_owned = 0;
    s->state = GIULIA_LINK_STOPPED;
    return error;
}

int giulia_pcie_link_prepare_parf(struct giulia_pcie_link *s, uint32_t flags,
    const struct giulia_pcie_link_io *io)
{
    struct giulia_pcie_link_io copy;
    uint32_t v;
    if (!s || !io || !io->read32 || !io->write32 || !io->read16 ||
        !io->write16 || !io->barrier || !io->delay_us)
        return GIULIA_LINK_ARGUMENT;
    if ((flags & GIULIA_LINK_PARF_REQUIRED) != GIULIA_LINK_PARF_REQUIRED ||
        (flags & (GIULIA_LINK_PERST_RELEASED_SETTLED | GIULIA_LINK_PHY_READY)))
        return GIULIA_LINK_PREREQUISITE;
    if (s->state != GIULIA_LINK_IDLE && s->state != GIULIA_LINK_STOPPED)
        return GIULIA_LINK_STATE;
    copy = *io;
    *s = (struct giulia_pcie_link){0};
    s->io = copy;
    s->stage = GIULIA_LINK_STAGE_ADMISSION;
    /* Only PARF may be accessed before cold CPU aperture setup. Flags assert
     * the caller already owns RC reset and has physically asserted PERST#. */
    v = r32(s, GIULIA_LINK_PARF, PARF_LTSSM);
    if (v == 0xffffffffu) return reject(s, GIULIA_LINK_READBACK);
    if (v & BIT(8)) return reject(s, GIULIA_LINK_BUSY);
    v = r32(s, GIULIA_LINK_PARF, PARF_INT_MASK);
    if (v == 0xffffffffu) return reject(s, GIULIA_LINK_READBACK);
    if (v) {
        if (!(flags & GIULIA_LINK_COLD_RESET_MASK))
            return reject(s, GIULIA_LINK_BUSY);
        s->mask_int_owned = 1;
    }
    if (!snapshot_parf(s)) return reject(s, GIULIA_LINK_READBACK);
    s->parf_owned = 1;
    s->stage = GIULIA_LINK_STAGE_PARF;
    if (!configure_parf(s)) return reject_after_parf(s, GIULIA_LINK_READBACK);
    s->state = GIULIA_LINK_PARF_READY;
    return GIULIA_LINK_OK;
}

int giulia_pcie_link_prepare(struct giulia_pcie_link *s, uint32_t flags)
{
    uint16_t unused_msi, unused_msix;
    uint32_t v;
    if (!s) return GIULIA_LINK_ARGUMENT;
    if ((flags & GIULIA_LINK_PREPARE_REQUIRED) != GIULIA_LINK_PREPARE_REQUIRED ||
        (flags & (GIULIA_LINK_PERST_RELEASED_SETTLED | GIULIA_LINK_PHY_QUIET)))
        return GIULIA_LINK_PREREQUISITE;
    if (s->state != GIULIA_LINK_PARF_READY || !s->parf_owned)
        return GIULIA_LINK_STATE;
    s->stage = GIULIA_LINK_STAGE_ADMISSION;
    s->root_id = r32(s, GIULIA_LINK_DBI, 0);
    if (s->root_id == 0xffffffffu) return record_failure(s, GIULIA_LINK_READBACK);
    if ((s->root_id & 0xffffu) != 0x17cbu ||
        (r16(s, GIULIA_LINK_DBI, PCI_HEADER) & 0x7fu) != 1u)
        return record_failure(s, GIULIA_LINK_ENDPOINT);
    if (!capabilities(s, GIULIA_LINK_DBI, &s->root_pcie_cap,
        &unused_msi, &unused_msix)) return record_failure(s, GIULIA_LINK_CAPABILITY);
    /* Link Control 2 exists in v2+ and must fit conventional config space.
     * Reject before any core write; do not reinterpret a v1/short capability. */
    if (s->root_pcie_cap > 0xccu)
        return record_failure(s, GIULIA_LINK_CAPABILITY);
    v = r16(s, GIULIA_LINK_DBI, s->root_pcie_cap + 2u);
    if (v == 0xffffu) return record_failure(s, GIULIA_LINK_READBACK);
    if ((v & 0xfu) < 2u) return record_failure(s, GIULIA_LINK_CAPABILITY);
    if (link_up(s)) return record_failure(s, GIULIA_LINK_BUSY);
    if (s->link_status == 0xffffu) return record_failure(s, GIULIA_LINK_READBACK);
    v = r16(s, GIULIA_LINK_DBI, PCI_COMMAND);
    if (v == 0xffffu) return record_failure(s, GIULIA_LINK_READBACK);
    if (v & BIT(2)) return record_failure(s, GIULIA_LINK_BUSY);
    /* The fixed 4 KiB unrolled aperture covers eight outbound/inbound pairs.
     * Refuse all active windows, not just the two that this probe will use. */
    for (unsigned n = 0; n < 8; n++) {
        uint32_t out = r32(s, GIULIA_LINK_ATU, n * 0x200u + ATU_CTRL2);
        uint32_t in = r32(s, GIULIA_LINK_ATU, n * 0x200u + 0x100u + ATU_CTRL2);
        uint32_t msi = r32(s, GIULIA_LINK_DBI, DBI_MSI_ENABLE + n * 12u);
        if (out == 0xffffffffu || in == 0xffffffffu || msi == 0xffffffffu)
            return record_failure(s, GIULIA_LINK_READBACK);
        if ((out & ATU_ENABLE) || (in & ATU_ENABLE) || msi)
            return record_failure(s, GIULIA_LINK_BUSY);
    }
    if (!snapshot_core(s)) return record_failure(s, GIULIA_LINK_READBACK);
    s->core_owned = 1;
    /* LTSSM snapshot is the one additional PARF entry in the core snapshot. */
    s->parf_count++;
    s->stage = GIULIA_LINK_STAGE_ROOT;
    v = r32(s, GIULIA_LINK_DBI, DBI_RO);
    if (v == 0xffffffffu ||
        !put32(s, GIULIA_LINK_DBI, DBI_RO, v | BIT(0), BIT(0)))
        return fail(s, GIULIA_LINK_READBACK);
#define DWRITE(o, value, mask) do { if (!put32(s, GIULIA_LINK_DBI, (o), \
    (value), (mask))) return fail(s, GIULIA_LINK_READBACK); } while (0)
#define DCHANGE(o, clear, set, mask) do { if (!change32(s, GIULIA_LINK_DBI, \
    (o), (clear), (set), (mask))) return fail(s, GIULIA_LINK_READBACK); } while (0)
    DWRITE(PCI_BAR0, 0, 0xfffffff0u);
    DWRITE(PCI_BAR1, 0, 0xffffffffu);
    DCHANGE(PCI_BUS, 0x00ffffffu, 0x00010100u, 0xffffffffu);
    /* A closed bridge memory window until the endpoint BAR is validated. */
    DWRITE(PCI_MEM, 0x0000fff0u, 0xfff0fff0u);
    if (!change16(s, GIULIA_LINK_DBI, PCI_COMMAND, 7u, BIT(10)) ||
        !put16(s, GIULIA_LINK_DBI, PCI_CLASS, 0x0604u))
        return fail(s, GIULIA_LINK_READBACK);
    /* A fixed Gen1 x1 budget must not inherit a Gen2/3 target or x2 width.
     * Program cold controls only; bit17 is an action, not saved configuration. */
    DCHANGE(DBI_LINK, (0x3fu << 16) | BIT(7), (1u << 16) | BIT(5), 0xffffffffu);
    DCHANGE(DBI_WIDTH, (0x1fu << 8) | BIT(17), GIULIA_PCIE_BOOT_LANES << 8, ~BIT(17));
    DCHANGE(s->root_pcie_cap + LNKCAP, 0x3ffu,
        (GIULIA_PCIE_BOOT_LANES << 4) | GIULIA_PCIE_BOOT_SPEED, 0xffffffffu);
    if (!change16(s, GIULIA_LINK_DBI, s->root_pcie_cap + LNKCTL2,
            0x0fu, GIULIA_PCIE_BOOT_SPEED)) return fail(s, GIULIA_LINK_READBACK);
    DCHANGE(s->root_pcie_cap + SLTCAP, BIT(6), 0, 0xffffffffu);
    if (!change16(s, GIULIA_LINK_DBI, s->root_pcie_cap + LNKCTL, 3u, 0))
        return fail(s, GIULIA_LINK_READBACK);
    DWRITE(DBI_RO, v, BIT(0));
#undef DWRITE
#undef DCHANGE
    s->state = GIULIA_LINK_PREPARED;
    return GIULIA_LINK_OK;
}

static int program_atu(struct giulia_pcie_link *s, unsigned region,
    uint32_t cpu, uint32_t target, uint32_t size, uint32_t type)
{
    uint32_t base = region * 0x200u;
    const uint32_t offsets[] = {ATU_CTRL2, ATU_BASE_LO, ATU_BASE_HI,
        ATU_LIMIT, ATU_LIMIT_HI, ATU_TARGET_LO, ATU_TARGET_HI, ATU_CTRL1};
    uint32_t values[] = {0, cpu, 0, cpu + size - 1u, 0, target, 0, type};
    uint32_t version = r32(s, GIULIA_LINK_DBI, DBI_VERSION);
    if (version == 0xffffffffu) return 0;
    /* DesignWare 4.90a needs the TD workaround even with ECRC disabled. */
    if (version == 0x3439302au)
        values[7] |= BIT(8);
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
        if (!put32(s, GIULIA_LINK_ATU, base + offsets[i], values[i],
            0xffffffffu)) return 0;
    w32(s, GIULIA_LINK_ATU, base + ATU_CTRL2, ATU_ENABLE);
    for (unsigned i = 0; i < 5; i++) {
        uint32_t got = r32(s, GIULIA_LINK_ATU, base + ATU_CTRL2);
        if (got == 0xffffffffu) return 0;
        if (got & ATU_ENABLE)
            return 1;
        delay(s, 9000);
    }
    return 0;
}

static int endpoint_quiet(struct giulia_pcie_link *s)
{
    uint16_t v;
    if (!cr16(s, PCI_COMMAND, &v) ||
        !cp16(s, PCI_COMMAND, (uint16_t)((v & ~7u) | BIT(10)))) return 0;
    if (s->endpoint_msi_cap) {
        if (!cr16(s, s->endpoint_msi_cap + 2u, &v) ||
            !cp16(s, s->endpoint_msi_cap + 2u, (uint16_t)(v & ~BIT(0))))
            return 0;
    }
    if (s->endpoint_msix_cap) {
        if (!cr16(s, s->endpoint_msix_cap + 2u, &v) ||
            !cp16(s, s->endpoint_msix_cap + 2u,
                (uint16_t)((v & ~BIT(15)) | BIT(14)))) return 0;
    }
    return 1;
}

static int probe_bar(struct giulia_pcie_link *s)
{
    uint32_t lo, hi, got_lo, got_hi;
    uint64_t mask, size;
    if (!cr32(s, PCI_BAR0, &lo) || !cr32(s, PCI_BAR1, &hi))
        return GIULIA_LINK_DOWN;
    /* The identified WCN7850 exposes a 64-bit non-prefetchable memory BAR. */
    if ((lo & 0xfu) != 4u) return GIULIA_LINK_BAR;
    if (!cw32(s, PCI_BAR0, 0xffffffffu) ||
        !cw32(s, PCI_BAR1, 0xffffffffu) ||
        !cr32(s, PCI_BAR0, &s->bar0_mask) ||
        !cr32(s, PCI_BAR1, &s->bar0_mask_hi)) return GIULIA_LINK_DOWN;
    /* Decode is off throughout; restore the probe even if size is wrong. */
    if (!cw32(s, PCI_BAR1, hi) || !cw32(s, PCI_BAR0, lo))
        return GIULIA_LINK_DOWN;
    mask = ((uint64_t)s->bar0_mask_hi << 32) | (s->bar0_mask & ~0xfu);
    size = ~mask + 1u;
    if ((s->bar0_mask & 0xfu) != 4u || size != GIULIA_PCIE_LINK_BAR0_SIZE)
        return GIULIA_LINK_BAR;
    if (!cw32(s, PCI_BAR1, 0) ||
        !cw32(s, PCI_BAR0, GIULIA_PCIE_LINK_BAR0_PA | 4u) ||
        !cr32(s, PCI_BAR0, &got_lo) || !cr32(s, PCI_BAR1, &got_hi))
        return GIULIA_LINK_DOWN;
    if (got_lo != (GIULIA_PCIE_LINK_BAR0_PA | 4u) || got_hi != 0)
        return GIULIA_LINK_READBACK;
    return GIULIA_LINK_OK;
}

int giulia_pcie_link_train(struct giulia_pcie_link *s, uint32_t flags)
{
    uint16_t header;
    int error;
    if (!s) return GIULIA_LINK_ARGUMENT;
    if ((flags & GIULIA_LINK_TRAIN_REQUIRED) != GIULIA_LINK_TRAIN_REQUIRED ||
        (flags & GIULIA_LINK_PERST_ASSERTED)) return GIULIA_LINK_PREREQUISITE;
    if (s->state != GIULIA_LINK_PREPARED) return GIULIA_LINK_STATE;
    s->stage = GIULIA_LINK_STAGE_TRAIN;
    if (!change32(s, GIULIA_LINK_PARF, PARF_LTSSM, 0, BIT(8), BIT(8)))
        return fail(s, GIULIA_LINK_READBACK);
    for (s->polls = 0; s->polls <= 1000u; s->polls++) {
        if (link_up(s)) break;
        if (s->polls < 1000u) delay(s, 1000);
    }
    s->debug0 = r32(s, GIULIA_LINK_DBI, DBI_DEBUG0);
    s->debug1 = r32(s, GIULIA_LINK_DBI, DBI_DEBUG1);
    if (s->polls > 1000u) return fail(s, GIULIA_LINK_TIMEOUT);
    if (!profile_matches(s)) return fail(s, GIULIA_LINK_PROFILE);
    s->stage = GIULIA_LINK_STAGE_CONFIG_ATU;
    s->atu_touched = 1;
    if (!program_atu(s, 0, GIULIA_PCIE_LINK_CFG_PA, 0x01000000u,
        GIULIA_PCIE_LINK_CFG_SIZE, 4)) return fail(s, GIULIA_LINK_READBACK);
    s->config_atu_enabled = 1;
    s->stage = GIULIA_LINK_STAGE_ENDPOINT;
    /* Allow the endpoint's reset/configuration readiness interval. */
    for (unsigned i = 0; i <= 100; i++) {
        if (!cr32(s, 0, &s->endpoint_id)) return fail(s, GIULIA_LINK_DOWN);
        if (s->endpoint_id == GIULIA_PCIE_LINK_EP_ID) break;
        if (s->endpoint_id != 0xffffffffu && s->endpoint_id != 0 &&
            (s->endpoint_id & 0xffffu) != 1u)
            return fail(s, GIULIA_LINK_ENDPOINT);
        if (i == 100) return fail(s, GIULIA_LINK_ENDPOINT);
        delay(s, 1000);
    }
    if (!cr16(s, PCI_HEADER, &header) || (header & 0x7fu) != 0)
        return fail(s, GIULIA_LINK_ENDPOINT);
    if (!capabilities(s, GIULIA_LINK_CFG, &s->endpoint_pcie_cap,
        &s->endpoint_msi_cap, &s->endpoint_msix_cap) || !s->endpoint_msi_cap)
        return fail(s, GIULIA_LINK_CAPABILITY);
    if (!endpoint_quiet(s)) return fail(s, GIULIA_LINK_READBACK);
    s->stage = GIULIA_LINK_STAGE_BAR;
    error = probe_bar(s);
    if (error) return fail(s, error);
    s->stage = GIULIA_LINK_STAGE_MEMORY_ATU;
    if (!program_atu(s, 1, GIULIA_PCIE_LINK_BAR0_PA, GIULIA_PCIE_LINK_BAR0_PA,
        GIULIA_PCIE_LINK_BAR0_SIZE, 0)) return fail(s, GIULIA_LINK_READBACK);
    /* Bridge memory window 0x60400000..0x605fffff, but both command MEM bits
     * remain zero until the explicit CPU-only MEM API is called. */
    if (!put32(s, GIULIA_LINK_DBI, PCI_MEM, 0x60506040u, 0xfff0fff0u) ||
        !endpoint_quiet(s)) return fail(s, GIULIA_LINK_READBACK);
    if (r16(s, GIULIA_LINK_DBI, PCI_COMMAND) & 7u)
        return fail(s, GIULIA_LINK_READBACK);
    s->stage = GIULIA_LINK_STAGE_READY;
    s->state = GIULIA_LINK_READY;
    return GIULIA_LINK_OK;
}

static int atu_matches(struct giulia_pcie_link *s, unsigned region,
    uint32_t cpu, uint32_t target, uint32_t size, uint32_t type)
{
    const uint32_t offsets[] = {ATU_CTRL1, ATU_CTRL2, ATU_BASE_LO,
        ATU_BASE_HI, ATU_LIMIT, ATU_TARGET_LO, ATU_TARGET_HI, ATU_LIMIT_HI};
    uint32_t expected[] = {type, ATU_ENABLE, cpu, 0, cpu + size - 1u,
        target, 0, 0};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        uint32_t got = r32(s, GIULIA_LINK_ATU, region * 0x200u + offsets[i]);
        uint32_t mask = i == 0 ? ~BIT(8) : 0xffffffffu; /* 4.90a TD workaround */
        if (got == 0xffffffffu || ((got ^ expected[i]) & mask)) return 0;
    }
    return 1;
}

static int config_window_matches(struct giulia_pcie_link *s)
{
    return s->config_atu_enabled && atu_matches(s, 0, GIULIA_PCIE_LINK_CFG_PA,
        0x01000000u, GIULIA_PCIE_LINK_CFG_SIZE, 4);
}

/* Read-only admission for CPU decoding. An enabled DMA/IRQ source is a
 * conflict, not permission to take it over or silently disable another user. */
static int memory_guard(struct giulia_pcie_link *s, int enabling,
    uint16_t *root_command, uint16_t *endpoint_command)
{
    uint16_t v;
    uint32_t d;
    if (!config_window_matches(s)) return GIULIA_LINK_READBACK;
    if (!link_up(s)) return s->link_status == 0xffffu ?
        GIULIA_LINK_READBACK : GIULIA_LINK_DOWN;
    if (!profile_matches(s)) return GIULIA_LINK_PROFILE;
    *root_command = r16(s, GIULIA_LINK_DBI, PCI_COMMAND);
    if (*root_command == 0xffffu || !cr16(s, PCI_COMMAND, endpoint_command))
        return GIULIA_LINK_READBACK;
    if ((*root_command & 5u) || (*endpoint_command & 5u) ||
        !(*root_command & BIT(10)) || !(*endpoint_command & BIT(10)))
        return GIULIA_LINK_BUSY;
    if (!cr16(s, s->endpoint_msi_cap + 2u, &v)) return GIULIA_LINK_READBACK;
    if (v & BIT(0)) return GIULIA_LINK_BUSY;
    if (s->endpoint_msix_cap) {
        if (!cr16(s, s->endpoint_msix_cap + 2u, &v)) return GIULIA_LINK_READBACK;
        if (v & BIT(15)) return GIULIA_LINK_BUSY;
    }
    for (unsigned i = 0; i < 8; i++) {
        d = r32(s, GIULIA_LINK_DBI, DBI_MSI_ENABLE + i * 12u);
        if (d == 0xffffffffu) return GIULIA_LINK_READBACK;
        if (d) return GIULIA_LINK_BUSY;
    }
    d = r32(s, GIULIA_LINK_PARF, PARF_INT_MASK);
    if (d == 0xffffffffu) return GIULIA_LINK_READBACK;
    if (d) return GIULIA_LINK_BUSY;
    if (!enabling) return GIULIA_LINK_OK;
    if (!atu_matches(s, 1, GIULIA_PCIE_LINK_BAR0_PA, GIULIA_PCIE_LINK_BAR0_PA,
        GIULIA_PCIE_LINK_BAR0_SIZE, 0)) return GIULIA_LINK_READBACK;
    d = r32(s, GIULIA_LINK_DBI, PCI_MEM);
    if (d == 0xffffffffu || (d & 0xfff0fff0u) != 0x60506040u)
        return GIULIA_LINK_READBACK;
    d = r32(s, GIULIA_LINK_DBI, PCI_BUS);
    if (d == 0xffffffffu || (d & 0x00ffffffu) != 0x00010100u)
        return GIULIA_LINK_READBACK;
    if (!cr32(s, 0, &d) || d != GIULIA_PCIE_LINK_EP_ID)
        return GIULIA_LINK_ENDPOINT;
    if (!cr32(s, PCI_BAR0, &d) || d != (GIULIA_PCIE_LINK_BAR0_PA | 4u) ||
        !cr32(s, PCI_BAR1, &d) || d != 0)
        return GIULIA_LINK_BAR;
    if (((*root_command & BIT(1)) != 0) != (s->memory_enabled != 0) ||
        ((*endpoint_command & BIT(1)) != 0) != (s->memory_enabled != 0))
        return GIULIA_LINK_BUSY;
    return GIULIA_LINK_OK;
}

static int clear_cpu_memory(struct giulia_pcie_link *s)
{
    uint16_t v;
    int endpoint_okay = 0;
    int root_okay;
    /* Never follow a corrupted CFG0 window. Still close the root side when
     * possible; PERST# + stop is required if the endpoint cannot be verified. */
    if (config_window_matches(s) && cr16(s, PCI_COMMAND, &v))
        endpoint_okay = cp16(s, PCI_COMMAND, (uint16_t)((v & ~7u) | BIT(10)));
    root_okay = change16(s, GIULIA_LINK_DBI, PCI_COMMAND, 7u, BIT(10));
    if (endpoint_okay && root_okay) s->memory_enabled = 0;
    return endpoint_okay && root_okay;
}

int giulia_pcie_link_mem_enable(struct giulia_pcie_link *s, uint32_t flags)
{
    uint16_t root_command, endpoint_command;
    int error;
    if (!s) return GIULIA_LINK_ARGUMENT;
    if ((flags & GIULIA_LINK_MEMORY_REQUIRED) != GIULIA_LINK_MEMORY_REQUIRED ||
        (flags & GIULIA_LINK_PERST_ASSERTED)) return GIULIA_LINK_PREREQUISITE;
    if (s->state != GIULIA_LINK_READY || !s->core_owned)
        return GIULIA_LINK_STATE;
    s->stage = GIULIA_LINK_STAGE_MEMORY_ENABLE;
    error = memory_guard(s, 1, &root_command, &endpoint_command);
    if (error) return record_failure(s, error);
    if (s->memory_enabled) {
        s->stage = GIULIA_LINK_STAGE_READY;
        return GIULIA_LINK_OK;
    }
    s->memory_owned = 1; /* A failed readback may still have changed hardware. */
    if (!put16(s, GIULIA_LINK_DBI, PCI_COMMAND,
            (uint16_t)((root_command & ~7u) | BIT(10) | BIT(1))) ||
        !cp16(s, PCI_COMMAND,
            (uint16_t)((endpoint_command & ~7u) | BIT(10) | BIT(1)))) {
        (void)clear_cpu_memory(s);
        return record_failure(s, GIULIA_LINK_READBACK);
    }
    s->memory_enabled = 1;
    s->stage = GIULIA_LINK_STAGE_READY;
    return GIULIA_LINK_OK;
}

int giulia_pcie_link_mem_disable(struct giulia_pcie_link *s, uint32_t flags)
{
    uint16_t root_command, endpoint_command;
    int error;
    if (!s) return GIULIA_LINK_ARGUMENT;
    if ((flags & GIULIA_LINK_MEMORY_REQUIRED) != GIULIA_LINK_MEMORY_REQUIRED ||
        (flags & GIULIA_LINK_PERST_ASSERTED)) return GIULIA_LINK_PREREQUISITE;
    if (!s->core_owned || (s->state != GIULIA_LINK_READY &&
        !(s->state == GIULIA_LINK_FAULT && s->memory_owned)))
        return GIULIA_LINK_STATE;
    s->stage = GIULIA_LINK_STAGE_MEMORY_DISABLE;
    error = memory_guard(s, 0, &root_command, &endpoint_command);
    if (error) {
        if (error != GIULIA_LINK_BUSY) (void)clear_cpu_memory(s);
        return record_failure(s, error);
    }
    if (!s->memory_owned && ((root_command | endpoint_command) & BIT(1)))
        return record_failure(s, GIULIA_LINK_BUSY);
    if (s->memory_owned && !clear_cpu_memory(s))
        return record_failure(s, GIULIA_LINK_READBACK);
    s->memory_enabled = 0;
    if (s->state == GIULIA_LINK_READY) s->stage = GIULIA_LINK_STAGE_READY;
    return GIULIA_LINK_OK;
}

int giulia_pcie_link_quiesce(struct giulia_pcie_link *s, uint32_t flags)
{
    unsigned i;
    if (!s) return GIULIA_LINK_ARGUMENT;
    if (s->state == GIULIA_LINK_IDLE || s->state == GIULIA_LINK_STOPPED)
        return GIULIA_LINK_OK;
    if ((flags & GIULIA_LINK_QUIESCE_REQUIRED) != GIULIA_LINK_QUIESCE_REQUIRED ||
        (flags & GIULIA_LINK_PERST_RELEASED_SETTLED))
        return GIULIA_LINK_PREREQUISITE;
    if (s->state != GIULIA_LINK_PREPARED && s->state != GIULIA_LINK_READY &&
        s->state != GIULIA_LINK_FAULT && s->state != GIULIA_LINK_QUIESCED &&
        s->state != GIULIA_LINK_PARF_READY)
        return GIULIA_LINK_STATE;
    if (s->state == GIULIA_LINK_QUIESCED) return GIULIA_LINK_OK;
    if (s->core_owned && (!(flags & GIULIA_LINK_PHY_READY) ||
        (flags & GIULIA_LINK_PHY_QUIET))) return GIULIA_LINK_PREREQUISITE;
    s->stage = GIULIA_LINK_STAGE_QUIESCE;
    if (!s->parf_owned) return record_failure(s, GIULIA_LINK_STATE);
    if (!s->core_owned) {
        /* This owner never touched or started the core; do not borrow it. */
        s->state = GIULIA_LINK_QUIESCED;
        return GIULIA_LINK_OK;
    }
    /* PERST# physically isolates the endpoint, including if config space was
     * lost during CPU MEM disable. Close the root window without touching the
     * reset endpoint before disabling LTSSM and restoring the snapshots. */
    if (!change16(s, GIULIA_LINK_DBI, PCI_COMMAND, 7u, BIT(10)))
        return record_failure(s, GIULIA_LINK_READBACK);
    if (!quiesce_controller(s)) return record_failure(s, GIULIA_LINK_READBACK);
    s->memory_enabled = 0;
    for (i = 0; i <= 100; i++) {
        int active = link_up(s);
        if (s->link_status == 0xffffu)
            return record_failure(s, GIULIA_LINK_READBACK);
        if (!active) break;
        if (i < 100) delay(s, 1000);
    }
    if (i > 100) return record_failure(s, GIULIA_LINK_TIMEOUT);
    if (!change32(s, GIULIA_LINK_DBI, DBI_RO, 0, BIT(0), BIT(0)) ||
        !restore_saved(s, s->parf_count, s->saved_count))
        return record_failure(s, GIULIA_LINK_READBACK);
    s->core_owned = s->memory_owned = s->atu_touched = 0;
    s->state = GIULIA_LINK_QUIESCED;
    return GIULIA_LINK_OK;
}

int giulia_pcie_link_stop(struct giulia_pcie_link *s, uint32_t flags)
{
    if (!s) return GIULIA_LINK_ARGUMENT;
    if (s->state == GIULIA_LINK_IDLE || s->state == GIULIA_LINK_STOPPED)
        return GIULIA_LINK_OK;
    if ((flags & GIULIA_LINK_STOP_REQUIRED) != GIULIA_LINK_STOP_REQUIRED ||
        (flags & (GIULIA_LINK_PERST_RELEASED_SETTLED | GIULIA_LINK_PHY_READY)))
        return GIULIA_LINK_PREREQUISITE;
    if (s->core_owned || (s->state != GIULIA_LINK_FAULT &&
        s->state != GIULIA_LINK_QUIESCED && s->state != GIULIA_LINK_PARF_READY))
        return GIULIA_LINK_STATE;
    s->stage = GIULIA_LINK_STAGE_STOP;
    if (!s->parf_owned) return record_failure(s, GIULIA_LINK_STATE);
    if (!restore_saved(s, 0, s->parf_count))
        return record_failure(s, GIULIA_LINK_READBACK);
    s->parf_owned = 0;
    s->state = GIULIA_LINK_STOPPED;
    return GIULIA_LINK_OK;
}
