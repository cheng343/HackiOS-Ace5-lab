/* SPDX-License-Identifier: BSD-2-Clause */
/* Original Ace 5 QMP PHY lifecycle. Register/protocol references:
 * Linux v6.12 phy-qcom-qmp-pcie.c: sm8650-qmp-gen3x2-pcie-phy uses
 * sm8550_qmp_gen3x2_pciephy_cfg, the v5 layout and PHYSTATUS bit6;
 * qmp_pcie_power_on/off define PIPE/reset/start ordering and 10ms timeout.
 * https://github.com/torvalds/linux/blob/v6.12/drivers/phy/qualcomm/phy-qcom-qmp-pcie.c
 * Per-board tuning facts and provenance: giulia_pcie_phy_table.h.
 */
#include "giulia_pcie_phy.h"
#include "giulia_pcie_phy_table.h"

#define PHY_RESET 0x200u
#define PHY_STATUS 0x214u
#define PHY_POWER 0x240u
#define PHY_START 0x244u
#define PHY_BUSY_BIT (1u << 6)

_Static_assert(GIULIA_PCIE_PHY_SETTINGS <= GIULIA_PHY_SAVE_MAX, "PHY snapshot capacity");

static int phy_update(struct giulia_pcie_phy *p, uint32_t off,
    uint32_t mask, uint32_t bits)
{
    uint32_t old = p->io.read32(p->io.cookie, off);
    uint32_t readback;
    if (old == UINT32_MAX) return GIULIA_PHY_READBACK;
    p->io.write32(p->io.cookie, off, (old & ~mask) | (bits & mask));
    p->io.barrier(p->io.cookie);
    readback = p->io.read32(p->io.cookie, off);
    return readback != UINT32_MAX && (readback & mask) == (bits & mask) ?
        GIULIA_PHY_OK : GIULIA_PHY_READBACK;
}

int giulia_pcie_phy_stop(struct giulia_pcie_phy *p, uint32_t prerequisites)
{
    unsigned i;
    if (!p) return GIULIA_PHY_ARGUMENT;
    if (p->state == GIULIA_PHY_IDLE || p->state == GIULIA_PHY_STOPPED)
        return GIULIA_PHY_OK;
    if ((prerequisites & GIULIA_PHY_STOP_REQUIRED) != GIULIA_PHY_STOP_REQUIRED)
        return GIULIA_PHY_PREREQUISITE;
    /* Same order as QMP power_off: PIPE first, CFG clock stays alive. */
    if (p->pipe_owned) {
        if (p->io.pipe_disable(p->io.cookie)) {
            p->state = GIULIA_PHY_FAULT;
            return GIULIA_PHY_PIPE;
        }
        p->pipe_owned = 0;
    }
    if (phy_update(p, PHY_RESET, 1u, 1u) || phy_update(p, PHY_START, 3u, 0))
        goto fault;
    /* A cold, quiescent PHY was admitted. Restore only tuning we changed,
     * while reset is asserted; never restart a previous firmware owner. */
    for (i = p->saved_count; i > 0; --i) {
        uint32_t off = giulia_pcie_phy_settings[i - 1].offset;
        uint32_t readback;
        p->io.write32(p->io.cookie, off, p->saved[i - 1]);
        p->io.barrier(p->io.cookie);
        /* These table entries program an 8-bit configuration value. */
        readback = p->io.read32(p->io.cookie, off);
        if (readback == UINT32_MAX || (readback & 0xffu) != (p->saved[i - 1] & 0xffu))
            goto fault;
    }
    if (phy_update(p, PHY_POWER, 3u, p->saved_power) ||
        phy_update(p, PHY_START, 3u, p->saved_start) ||
        phy_update(p, PHY_RESET, 1u, p->saved_reset)) goto fault;
    p->saved_count = 0;
    p->state = GIULIA_PHY_STOPPED;
    return GIULIA_PHY_OK;
fault:
    p->state = GIULIA_PHY_FAULT;
    return GIULIA_PHY_READBACK;
}

static int phy_failed(struct giulia_pcie_phy *p, int error)
{
    p->failure = error;
    /* Start has not released PERST# or enabled any controller/DMA user. */
    (void)giulia_pcie_phy_stop(p, GIULIA_PHY_STOP_REQUIRED);
    return error;
}

int giulia_pcie_phy_start(struct giulia_pcie_phy *p, uint32_t prerequisites,
    const struct giulia_pcie_phy_io *io)
{
    uint32_t reset, start, power;
    unsigned i;
    if (!p || !io || !io->read32 || !io->write32 || !io->barrier ||
        !io->delay_us || !io->pipe_enable || !io->pipe_disable)
        return GIULIA_PHY_ARGUMENT;
    if (p->state != GIULIA_PHY_IDLE && p->state != GIULIA_PHY_STOPPED)
        return GIULIA_PHY_STATE;
    if ((prerequisites & GIULIA_PHY_START_REQUIRED) != GIULIA_PHY_START_REQUIRED)
        return GIULIA_PHY_PREREQUISITE;
    reset = io->read32(io->cookie, PHY_RESET);
    start = io->read32(io->cookie, PHY_START);
    power = io->read32(io->cookie, PHY_POWER);
    if (reset == UINT32_MAX || start == UINT32_MAX || power == UINT32_MAX)
        return GIULIA_PHY_READBACK;
    if ((start & 3u) || (power & 3u)) return GIULIA_PHY_BUSY;
    p->io = *io;
    p->saved_reset = reset;
    p->saved_start = start;
    p->saved_power = power;
    p->saved_count = p->pipe_owned = p->polls = p->last_status = 0;
    p->failure = 0;
    p->state = GIULIA_PHY_OWNED;
    if (phy_update(p, PHY_RESET, 1u, 1u) || phy_update(p, PHY_POWER, 3u, 3u))
        return phy_failed(p, GIULIA_PHY_READBACK);
    for (i = 0; i < GIULIA_PCIE_PHY_SETTINGS; ++i) {
        uint32_t off = giulia_pcie_phy_settings[i].offset;
        uint32_t readback;
        p->saved[i] = io->read32(io->cookie, off);
        if (p->saved[i] == UINT32_MAX) return phy_failed(p, GIULIA_PHY_READBACK);
        p->saved_count = i + 1;
        io->write32(io->cookie, off, giulia_pcie_phy_settings[i].value);
        io->barrier(io->cookie);
        readback = io->read32(io->cookie, off);
        if (readback == UINT32_MAX || (readback & 0xffu) != giulia_pcie_phy_settings[i].value)
            return phy_failed(p, GIULIA_PHY_READBACK);
    }
    io->barrier(io->cookie);
    const int pcs_first = (prerequisites & GIULIA_PHY_PCS_BEFORE_PIPE) != 0;
    if (pcs_first) {
        if (phy_update(p, PHY_RESET, 1u, 0) || phy_update(p, PHY_START, 3u, 3u))
            return phy_failed(p, GIULIA_PHY_READBACK);
        io->delay_us(io->cookie, 1000);
    }
    /* Mark before callback: a failed enable may have partially voted PIPE. */
    p->pipe_owned = 1;
    if (io->pipe_enable(io->cookie)) return phy_failed(p, GIULIA_PHY_PIPE);
    if (!pcs_first) {
        if (phy_update(p, PHY_RESET, 1u, 0) || phy_update(p, PHY_START, 3u, 3u))
            return phy_failed(p, GIULIA_PHY_READBACK);
        io->delay_us(io->cookie, 1000);
    }
    for (i = 0; i <= 50; ++i) {
        p->last_status = io->read32(io->cookie, PHY_STATUS);
        p->polls = i + 1;
        if (p->last_status == UINT32_MAX) return phy_failed(p, GIULIA_PHY_READBACK);
        if (!(p->last_status & PHY_BUSY_BIT)) {
            p->state = GIULIA_PHY_READY;
            return GIULIA_PHY_OK;
        }
        if (i < 50) io->delay_us(io->cookie, 200);
    }
    return phy_failed(p, GIULIA_PHY_TIMEOUT);
}
