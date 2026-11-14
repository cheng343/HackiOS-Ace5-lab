/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_PCIE_LINK_H
#define GIULIA_PCIE_LINK_H

#include <stdint.h>

/* SM8650 PCIe0, one directly attached WCN7850 at 01:00.0. The caller owns
 * mapping, power, NoC votes, PHY and PERST#. This core never enables DMA,
 * endpoint memory decoding, interrupts, MSI, SID bypass or SMMU bypass.
 * Call all operations on one serialized owner; never race MSI or BAR users.
 */
#define GIULIA_PCIE_LINK_DBI_PA 0x60000000u
#define GIULIA_PCIE_LINK_ATU_PA 0x60001000u
#define GIULIA_PCIE_LINK_CFG_PA 0x60100000u
#define GIULIA_PCIE_LINK_CFG_SIZE 0x00100000u
#define GIULIA_PCIE_LINK_BAR0_PA 0x60400000u
#define GIULIA_PCIE_LINK_BAR0_SIZE 0x00200000u
#define GIULIA_PCIE_LINK_EP_ID 0x110717cbu

enum giulia_pcie_link_region {
    GIULIA_LINK_PARF, GIULIA_LINK_DBI, GIULIA_LINK_ATU, GIULIA_LINK_CFG
};
enum giulia_pcie_link_result {
    GIULIA_LINK_OK = 0, GIULIA_LINK_ARGUMENT = -1,
    GIULIA_LINK_PREREQUISITE = -2, GIULIA_LINK_STATE = -3,
    GIULIA_LINK_BUSY = -4, GIULIA_LINK_CAPABILITY = -5,
    GIULIA_LINK_READBACK = -6, GIULIA_LINK_TIMEOUT = -7,
    GIULIA_LINK_ENDPOINT = -8, GIULIA_LINK_BAR = -9,
    GIULIA_LINK_DOWN = -10
};
enum giulia_pcie_link_state {
    GIULIA_LINK_IDLE, GIULIA_LINK_PREPARED, GIULIA_LINK_READY,
    GIULIA_LINK_FAULT, GIULIA_LINK_STOPPED
};
enum giulia_pcie_link_stage {
    GIULIA_LINK_STAGE_IDLE, GIULIA_LINK_STAGE_ADMISSION,
    GIULIA_LINK_STAGE_PARF, GIULIA_LINK_STAGE_ROOT,
    GIULIA_LINK_STAGE_TRAIN, GIULIA_LINK_STAGE_CONFIG_ATU,
    GIULIA_LINK_STAGE_ENDPOINT, GIULIA_LINK_STAGE_BAR,
    GIULIA_LINK_STAGE_MEMORY_ATU, GIULIA_LINK_STAGE_READY,
    GIULIA_LINK_STAGE_STOP
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
    GIULIA_LINK_USERS_QUIESCED = 1u << 6
};
#define GIULIA_LINK_PREPARE_REQUIRED (GIULIA_LINK_EXCLUSIVE | \
    GIULIA_LINK_RAILS_NOC_READY | GIULIA_LINK_CLOCKS_RC_RESET_READY | \
    GIULIA_LINK_PERST_ASSERTED)
#define GIULIA_LINK_TRAIN_REQUIRED (GIULIA_LINK_EXCLUSIVE | \
    GIULIA_LINK_RAILS_NOC_READY | GIULIA_LINK_CLOCKS_RC_RESET_READY | \
    GIULIA_LINK_PHY_READY | GIULIA_LINK_PERST_RELEASED_SETTLED)
#define GIULIA_LINK_STOP_REQUIRED (GIULIA_LINK_EXCLUSIVE | \
    GIULIA_LINK_RAILS_NOC_READY | GIULIA_LINK_CLOCKS_RC_RESET_READY | \
    GIULIA_LINK_PERST_ASSERTED | \
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
    uint32_t root_id, endpoint_id, link_status, debug0, debug1;
    uint32_t polls, bar0_mask, bar0_mask_hi;
    uint16_t root_pcie_cap, endpoint_pcie_cap, endpoint_msi_cap;
    uint16_t endpoint_msix_cap;
    uint8_t saved_count, config_atu_enabled, atu_touched;
    struct giulia_pcie_link_saved saved[48];
};

/* Zero-initialize once. Missing prerequisites cause no MMIO. prepare refuses
 * an enabled LTSSM, live link, enabled iATU region, PARF global IRQ source
 * or internal MSI receiver.
 * It must run before the PHY is powered on (PARF PHY_TEST_PWR_DOWN release).
 * A ready flag is the caller's observed hardware condition, not a substitute
 * for owning/enabling supplies, NoC and clocks. This is not a firmware-handoff
 * path: PERST# is asserted and the controller has been exclusively reset.
 * The caller must also confirm DBI and unrolled ATU apertures are readable
 * after that reset. This core snapshots/refuses foreign windows before PARF
 * reconfiguration. If reset leaves those apertures inaccessible it safely
 * refuses admission; an earlier board-owned PARF mapping phase is then needed.
 */
int giulia_pcie_link_prepare(struct giulia_pcie_link *, uint32_t prerequisites,
    const struct giulia_pcie_link_io *);
/* After PHY init and the documented PERST# timing, train for at most 1 second
 * and map CFG0. Endpoint ID, capability list, MSI disable, BAR size and assigned
 * address are read back. MEM iATU region 1 is prepared, but endpoint IO/MEM/BME
 * and root IO/BME remain disabled. READY therefore does not mean Wi-Fi works.
 * A failed operation keeps mappings/resources owned in FAULT; reset endpoint
 * then call stop. Configuration accesses can still SError on a physical link
 * loss between the link check and transaction; software cannot eliminate it.
 */
int giulia_pcie_link_train(struct giulia_pcie_link *, uint32_t prerequisites);
/* Caller first quiesces all upper layers and asserts PERST#. Restore only the
 * originally inactive controller/iATU snapshot. On readback failure retain
 * clocks, mappings and object in FAULT and retry; do not unmap or power down.
 * Before PHY teardown use stop; only then release PHY/clocks/NoC/rails.
 */
int giulia_pcie_link_stop(struct giulia_pcie_link *, uint32_t prerequisites);

#endif
