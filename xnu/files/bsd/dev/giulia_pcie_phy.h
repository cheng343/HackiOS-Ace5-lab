/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_PCIE_PHY_H
#define GIULIA_PCIE_PHY_H
#include <stdint.h>

/* SM8650 PCIe0 Gen3x2 QMP PHY, 0x01c06000..0x01c07fff.
 * Callback core: no mapping, clocks, GPIO or regulator ownership is inferred.
 * Caller serializes the lifecycle and retains all resources until STOPPED.
 */
enum giulia_pcie_phy_result {
    GIULIA_PHY_OK = 0, GIULIA_PHY_ARGUMENT = -1,
    GIULIA_PHY_PREREQUISITE = -2, GIULIA_PHY_STATE = -3,
    GIULIA_PHY_BUSY = -4, GIULIA_PHY_READBACK = -5,
    GIULIA_PHY_TIMEOUT = -6, GIULIA_PHY_PIPE = -7
};
enum giulia_pcie_phy_state {
    GIULIA_PHY_IDLE, GIULIA_PHY_OWNED, GIULIA_PHY_READY,
    GIULIA_PHY_FAULT, GIULIA_PHY_STOPPED, GIULIA_PHY_RESET_REQUIRED
};
enum giulia_pcie_phy_prerequisite {
    GIULIA_PHY_EXCLUSIVE = 1u << 0,
    GIULIA_PHY_POWER_CLOCKS_READY = 1u << 1,
    GIULIA_PHY_PERST_ASSERTED = 1u << 2,
    GIULIA_PHY_LINK_QUIET = 1u << 3,
    GIULIA_PHY_PARF_PREPARED = 1u << 4,
    /* Ace 5 downstream trial with PERST# held and independent reset armed. */
    GIULIA_PHY_PCS_BEFORE_PIPE = 1u << 5,
    /* A guarded early CESTA START owns the clocks until cold reset. */
    GIULIA_PHY_RETAIN_ON_START_FAILURE = 1u << 6
};
#define GIULIA_PHY_STOP_REQUIRED (GIULIA_PHY_EXCLUSIVE | \
    GIULIA_PHY_POWER_CLOCKS_READY | GIULIA_PHY_PERST_ASSERTED | GIULIA_PHY_LINK_QUIET)
#define GIULIA_PHY_START_REQUIRED (GIULIA_PHY_STOP_REQUIRED | GIULIA_PHY_PARF_PREPARED)
#define GIULIA_PHY_SAVE_MAX 128u

struct giulia_pcie_phy_io {
    void *cookie;
    uint32_t (*read32)(void *, uint32_t offset);
    void (*write32)(void *, uint32_t offset, uint32_t value);
    void (*barrier)(void *);
    void (*delay_us)(void *, uint32_t);
    /* Enable can fail after a partial change. disable must be retryable and
     * must keep CFG/AUX/reference/RCHNG clocks available for PHY registers. */
    int (*pipe_enable)(void *);
    int (*pipe_disable)(void *);
};
struct giulia_pcie_phy {
    struct giulia_pcie_phy_io io;
    uint32_t state, saved_reset, saved_start, saved_power;
    uint32_t saved[GIULIA_PHY_SAVE_MAX];
    uint32_t saved_count, last_status, polls;
    unsigned pipe_owned;
    int failure;
};

/* Zero-initialize once. Refuses an already powered/started PHY. Programs the
 * public Ace 5 tuning table, then PIPE, SW_RESET, START. The opt-in trial
 * releases SW_RESET and writes START 1ms before PIPE, as in pci-msm.
 * Polls PHYSTATUS bit6 for <=10ms. No firmware or DMA is involved. On failure,
 * cleanup is normally attempted before return; FAULT means retain/retry.
 * RETAIN_ON_START_FAILURE is valid only with PCS-before-PIPE. After an
 * attempted PHY write it forces RESET_REQUIRED instead of touching PIPE or
 * the snapshot again. Only a separate cold session can restart that PHY. */
int giulia_pcie_phy_start(struct giulia_pcie_phy *, uint32_t prerequisites,
    const struct giulia_pcie_phy_io *);
/* After upper users stop, assert PERST# and stop the controller first. Stop
 * PIPE, hold PHY reset, clear START, restore saved tuning and controls.
 * A failed stop retains the snapshot; it never reports resources released.
 * RESET_REQUIRED refuses this ordinary stop without touching hardware. */
int giulia_pcie_phy_stop(struct giulia_pcie_phy *, uint32_t prerequisites);
#endif
