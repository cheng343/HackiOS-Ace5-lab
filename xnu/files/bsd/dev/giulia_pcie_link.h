/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_PCIE_LINK_H
#define GIULIA_PCIE_LINK_H

#include <stdint.h>
#include "giulia_pcie_profile.h"

/* SM8650 PCIe0, one directly attached WCN7850 at 01:00.0. The caller owns
 * mapping, power, NoC votes, PHY and PERST#. This core never enables DMA,
 * interrupts, MSI, SID bypass or SMMU bypass. CPU BAR decoding is opt-in.
 * Call all operations on one serialized owner; never race MSI or BAR users.
 */
#define GIULIA_PCIE_LINK_DBI_PA 0x60000000u
#define GIULIA_PCIE_LINK_ATU_PA 0x60001000u
#define GIULIA_PCIE_LINK_CFG_PA 0x60100000u
#define GIULIA_PCIE_LINK_CFG_SIZE 0x00100000u
#define GIULIA_PCIE_LINK_BAR0_PA 0x60400000u
#define GIULIA_PCIE_LINK_BAR0_SIZE 0x00200000u
#define GIULIA_PCIE_LINK_ROOT_ID 0x011317cbu
#define GIULIA_PCIE_LINK_EP_ID 0x110717cbu

/* Validity of observations, not link-up conditions. PARF bit 8 enables
 * training; bits [5:0] expose the vendor LTSSM state. */
#define GIULIA_LINK_TRAIN_PARF_ENABLE_VALID (1u << 0)
#define GIULIA_LINK_TRAIN_PARF_TIMEOUT_VALID (1u << 1)

enum giulia_pcie_link_region {
    GIULIA_LINK_PARF, GIULIA_LINK_DBI, GIULIA_LINK_ATU, GIULIA_LINK_CFG
};
enum giulia_pcie_link_result {
    GIULIA_LINK_OK = 0, GIULIA_LINK_ARGUMENT = -1,
    GIULIA_LINK_PREREQUISITE = -2, GIULIA_LINK_STATE = -3,
    GIULIA_LINK_BUSY = -4, GIULIA_LINK_CAPABILITY = -5,
    GIULIA_LINK_READBACK = -6, GIULIA_LINK_TIMEOUT = -7,
    GIULIA_LINK_ENDPOINT = -8, GIULIA_LINK_BAR = -9,
    GIULIA_LINK_DOWN = -10, GIULIA_LINK_PROFILE = -11
};
enum giulia_pcie_link_state {
    GIULIA_LINK_IDLE, GIULIA_LINK_PREPARED, GIULIA_LINK_READY,
    GIULIA_LINK_FAULT, GIULIA_LINK_STOPPED, GIULIA_LINK_QUIESCED,
    GIULIA_LINK_PARF_READY, GIULIA_LINK_IDENTIFIED
};
enum giulia_pcie_link_stage {
    GIULIA_LINK_STAGE_IDLE, GIULIA_LINK_STAGE_ADMISSION,
    GIULIA_LINK_STAGE_PARF, GIULIA_LINK_STAGE_ROOT,
    GIULIA_LINK_STAGE_TRAIN, GIULIA_LINK_STAGE_CONFIG_ATU,
    GIULIA_LINK_STAGE_ENDPOINT, GIULIA_LINK_STAGE_BAR,
    GIULIA_LINK_STAGE_MEMORY_ATU, GIULIA_LINK_STAGE_READY,
    GIULIA_LINK_STAGE_STOP, GIULIA_LINK_STAGE_MEMORY_ENABLE,
    GIULIA_LINK_STAGE_MEMORY_DISABLE, GIULIA_LINK_STAGE_QUIESCE
};
enum giulia_pcie_link_prerequisite {
    GIULIA_LINK_EXCLUSIVE = 1u << 0,
    GIULIA_LINK_RAILS_NOC_READY = 1u << 1,
    GIULIA_LINK_CLOCKS_RC_RESET_READY = 1u << 2,
    GIULIA_LINK_PERST_ASSERTED = 1u << 3,
    GIULIA_LINK_PHY_READY = 1u << 4,
    /* PERST# was asserted >=100 ms, then released >=1 ms ago. */
    GIULIA_LINK_PERST_RELEASED_SETTLED = 1u << 5,
    /* No other config/BAR/MSI users remain; MSI stop succeeded first. */
    GIULIA_LINK_USERS_QUIESCED = 1u << 6,
    /* QMP stop completed, or this owner never started the PHY. */
    GIULIA_LINK_PHY_QUIET = 1u << 7,
    /* Caller just completed an exclusive GCC_PCIE_0_BCR cold reset: SM8650
     * resets PARF_INT_MASK (0x22c) non-zero. Snapshot and explicitly clear it
     * to 0 in configure_parf() instead of rejecting as a foreign active IRQ. */
    GIULIA_LINK_COLD_RESET_MASK = 1u << 8,
    GIULIA_LINK_EXACT_ROOT_ID = 1u << 9,
    /* Apply the stock pre-PERST controls and check PCIe0 ELBI link status. */
    GIULIA_LINK_VENDOR_TRAINING = 1u << 10,
    /* Preserve the native two-lane fields in a guarded developer trial. */
    GIULIA_LINK_NATIVE_WIDTH = 1u << 11,
    /* OnePlus RC0 vendor training aims NUM_OF_LANES=1 with native x2 capability. */
    GIULIA_LINK_STOCK_TRAIN_LANES = 1u << 12,
    /* Native x2 root, ID-only: pin the actually negotiated Gen1 width. */
    GIULIA_LINK_NATIVE_X1_ID = 1u << 13
};
#define GIULIA_LINK_PARF_REQUIRED (GIULIA_LINK_EXCLUSIVE | \
    GIULIA_LINK_RAILS_NOC_READY | GIULIA_LINK_CLOCKS_RC_RESET_READY | \
    GIULIA_LINK_PERST_ASSERTED)
#define GIULIA_LINK_PREPARE_REQUIRED (GIULIA_LINK_PARF_REQUIRED | GIULIA_LINK_PHY_READY)
#define GIULIA_LINK_TRAIN_REQUIRED (GIULIA_LINK_EXCLUSIVE | \
    GIULIA_LINK_RAILS_NOC_READY | GIULIA_LINK_CLOCKS_RC_RESET_READY | \
    GIULIA_LINK_PHY_READY | GIULIA_LINK_PERST_RELEASED_SETTLED)
#define GIULIA_LINK_QUIESCE_REQUIRED (GIULIA_LINK_EXCLUSIVE | \
    GIULIA_LINK_RAILS_NOC_READY | GIULIA_LINK_CLOCKS_RC_RESET_READY | \
    GIULIA_LINK_PERST_ASSERTED | \
    GIULIA_LINK_USERS_QUIESCED)
#define GIULIA_LINK_STOP_REQUIRED (GIULIA_LINK_QUIESCE_REQUIRED | GIULIA_LINK_PHY_QUIET)
#define GIULIA_LINK_MEMORY_REQUIRED (GIULIA_LINK_TRAIN_REQUIRED | \
    GIULIA_LINK_USERS_QUIESCED)

struct giulia_pcie_link_io {
    void *cookie;
    uint32_t (*read32)(void *, unsigned region, uint32_t offset);
    void (*write32)(void *, unsigned region, uint32_t offset, uint32_t value);
    uint16_t (*read16)(void *, unsigned region, uint32_t offset);
    void (*write16)(void *, unsigned region, uint32_t offset, uint16_t value);
    void (*barrier)(void *);
    void (*delay_us)(void *, uint32_t);
};
struct giulia_pcie_link_saved {
    uint32_t offset, value;
    uint8_t region, width;
};
struct giulia_pcie_link {
    struct giulia_pcie_link_io io;
    uint32_t state, stage, failure_stage;
    int error;
    uint32_t root_id, endpoint_id, link_status, debug0, debug1, elbi_status;
    /* Only paired, readable DBI/ELBI samples from the training loop.
     * First/last are meaningful when train_samples is nonzero. */
    uint32_t train_samples, train_ltssm_first, train_ltssm_last;
    uint32_t train_ltssm_seen[2];
    /* Historical snapshots only. Unset validity means absent or invalid;
     * consult the stage/error, not zero-filled fields, for early failures. */
    uint32_t train_parf_enable_raw, train_parf_timeout_raw, train_parf_valid;
    /* Counts from complete, valid, paired DBI/ELBI samples only. Ready
     * means DLLLA and XMLH together with LT clear; later CFG checks excluded. */
    uint32_t train_xmlh_samples, train_dllla_samples, train_lt_samples, train_ready_samples;
    uint32_t polls, bar0_mask, bar0_mask_hi;
    uint16_t root_pcie_cap, endpoint_pcie_cap, endpoint_msi_cap;
    uint16_t endpoint_msix_cap;
    uint8_t saved_count, config_atu_enabled, atu_touched;
    uint8_t parf_count, parf_owned, core_owned;
    uint8_t memory_owned, memory_enabled, mask_int_owned, vendor_training;
    uint8_t expected_lanes, stock_train, snapshot_lanes;
    uint8_t x1_id, id_lanes; /* latched only after paired ready at Gen1 x1/x2 */
    uint8_t train_lanes_before, train_lanes_after, train_lanes_verified;
    struct giulia_pcie_link_saved saved[48];
};

/* Zero-initialize once. Missing prerequisites cause no MMIO. PARF setup
 * must run before PHY power-on and refuses enabled LTSSM or PARF IRQs.
 * A ready flag is the caller's observed hardware condition, not a substitute
 * for owning/enabling supplies, NoC and clocks. This is not a firmware-handoff
 * path: PERST# is asserted and the controller has been exclusively reset.
 * Admission starts with PARF-only LTSSM/IRQ checks and a PARF snapshot. It
 * establishes DBI/ATU decoding before the first DBI/ATU transaction. The CPU
 * physical mappings above are distinct from the downstream PARF decoder's
 * aperture-local offsets (DBI=0, iATU=0x1000, slave size=64 MiB). The latter
 * profile is a candidate based on the vendor DBI convention, not a measured
 * guarantee that a core read will complete in every handoff state.
 * This does not borrow a running link.
 */
int giulia_pcie_link_prepare_parf(struct giulia_pcie_link *, uint32_t prerequisites,
    const struct giulia_pcie_link_io *);
/* Recheck the PARF decoder and perform exactly one read of DBI+0. Caller
 * keeps the PHY/PIPE and PARF lease, and provides independent reset. */
int giulia_pcie_link_read_root_id(struct giulia_pcie_link *, uint32_t prerequisites);
/* Enter DBI/ATU only after the PHY and PIPE clock are ready. A busy or
 * unreadable core leaves PARF owned until the PHY is stopped.
 */
int giulia_pcie_link_prepare(struct giulia_pcie_link *, uint32_t prerequisites);
/* Start the link and read the expected endpoint ID through CFG0. Retains the
 * root and CFG0 owner; performs no endpoint writes or BAR/MEM programming. */
int giulia_pcie_link_identify(struct giulia_pcie_link *, uint32_t prerequisites);
/* prepare fixes both target/max speed and all RC width controls to Gen1 x1,
 * matching the initial CRM resource profile. It requires a PCIe v2 root
 * capability with Link Control 2 in bounds. The x2 PHY can remain initialized;
 * the RC negotiates one lane. It does not request a later speed transition.
 * After PHY init and the documented PERST# timing, train for at most 1 second
 * and map CFG0. Endpoint ID, capability list, MSI disable, BAR size and assigned
 * address are read back. MEM iATU region 1 is prepared, but endpoint IO/MEM/BME
 * and root IO/BME remain disabled. READY therefore does not mean Wi-Fi works.
 * A negotiated speed/width outside this profile (including zero) is rejected
 * before opening CFG0. Each later endpoint transaction checks the profile too;
 * this does not remove the asynchronous-change race or guard raw caller BARs.
 * A failed operation keeps mappings/resources owned in FAULT; reset endpoint
 * then call stop. Configuration accesses can still SError on a physical link
 * loss between the link check and transaction; software cannot eliminate it.
 */
int giulia_pcie_link_train(struct giulia_pcie_link *, uint32_t prerequisites);
/* Explicit CPU-only BAR access. Enable requires READY, a live link, all other
 * BAR/config/MSI users quiesced, unchanged CFG/MEM windows and BAR0 assignment,
 * and both BME and MSI/MSI-X disabled. It enables root MEM before endpoint MEM;
 * IO/BME remain zero. This is not permission to map DMA or start MHI.
 * Disable closes endpoint MEM before root MEM. Readback/link errors retain
 * ownership in FAULT: retry disable while the link is accessible, or assert
 * PERST# and stop. memory_enabled is meaningful only in READY; FAULT may leave
 * decode partially enabled. No caller may access BARs after a failed API.
 */
int giulia_pcie_link_mem_enable(struct giulia_pcie_link *, uint32_t prerequisites);
int giulia_pcie_link_mem_disable(struct giulia_pcie_link *, uint32_t prerequisites);
/* After PERST# assertion, disable CPU MEM/LTSSM/ATU and confirm link down, but
 * restore owned DBI/ATU with PHY_READY and retain PARF. This must precede
 * QMP stop: restoring PARF_PHY_TEST_PWR_DOWN before PIPE cleanup is unsafe.
 * QUIESCED still owns mappings; it never authorizes caller resource release.
 * A provisional PARF-only owner leaves the unowned core entirely untouched.
 */
int giulia_pcie_link_quiesce(struct giulia_pcie_link *, uint32_t prerequisites);
/* Caller first quiesces all upper layers, asserts PERST#, calls quiesce and
 * stops QMP. PHY_QUIET is required before restoring PARF's power-down state.
 * Stop touches PARF only. On readback failure retain clocks, mappings and
 * PARF ownership in FAULT and retry; do not unmap or power down.
 * Release clocks/NoC/rails only after PHY stop and this final restore succeed.
 */
int giulia_pcie_link_stop(struct giulia_pcie_link *, uint32_t prerequisites);

#endif
