/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_WIFI_BOARD_H
#define GIULIA_WIFI_BOARD_H

#include "giulia_wifi_gpio.h"
#include "giulia_pcie_power.h"
#include "giulia_pcie_link.h"
#include "giulia_pcie_phy.h"

/* Serial coordinator for the real Ace 5 GPIO/GCC/PARF/QMP cores. It borrows
 * external supplies, modes/AOP completion, NoC and parent clocks; it neither
 * acquires nor withdraws those votes. No global instance or boot hook exists.
 */
enum giulia_wifi_board_condition {
    GIULIA_WBOARD_EXCLUSIVE = 1u << 0,
    GIULIA_WBOARD_MMIO_ACCESSIBLE = 1u << 1,
    GIULIA_WBOARD_RAILS_MODES_AOP_READY = 1u << 2,
    GIULIA_WBOARD_NOC_VOTES_READY = 1u << 3,
    GIULIA_WBOARD_CESTA_RELINQUISHED = 1u << 4,
    GIULIA_WBOARD_PARENT_CLOCKS_READY = 1u << 5,
    /* All external config/BAR/MSI/DMA users are stopped and serialized. */
    GIULIA_WBOARD_USERS_QUIESCED = 1u << 6,
    /* Caller is authorized to reset an inactive WLAN/BT/PCIe0 instance. */
    GIULIA_WBOARD_COLD_RESET_ALLOWED = 1u << 7
};
#define GIULIA_WBOARD_START_REQUIRED 0xffu
#define GIULIA_WBOARD_STOP_REQUIRED 0x7fu

enum giulia_wifi_board_result {
    GIULIA_WBOARD_OK = 0, GIULIA_WBOARD_ARGUMENT = -1,
    GIULIA_WBOARD_STATE = -2, GIULIA_WBOARD_PREREQUISITE = -3,
    GIULIA_WBOARD_EXTERNAL = -4, GIULIA_WBOARD_SUBSYSTEM = -5,
    GIULIA_WBOARD_CONTRACT = -6
};
enum giulia_wifi_board_state {
    GIULIA_WBOARD_IDLE, GIULIA_WBOARD_STARTING,
    GIULIA_WBOARD_CPU_BAR_READY, GIULIA_WBOARD_STOPPING,
    GIULIA_WBOARD_STOPPED, GIULIA_WBOARD_FAULT
};
enum giulia_wifi_board_stage {
    GIULIA_WBOARD_STAGE_NONE, GIULIA_WBOARD_STAGE_GPIO_BEGIN,
    GIULIA_WBOARD_STAGE_WLAN_ON, GIULIA_WBOARD_STAGE_POWER,
    GIULIA_WBOARD_STAGE_LINK_PREPARE, GIULIA_WBOARD_STAGE_PHY,
    GIULIA_WBOARD_STAGE_PERST_RELEASE, GIULIA_WBOARD_STAGE_LINK_TRAIN,
    GIULIA_WBOARD_STAGE_MEMORY, GIULIA_WBOARD_STAGE_ASSERT_PERST,
    GIULIA_WBOARD_STAGE_LINK_STOP, GIULIA_WBOARD_STAGE_PHY_STOP,
    GIULIA_WBOARD_STAGE_POWER_STOP, GIULIA_WBOARD_STAGE_GPIO_STOP,
    GIULIA_WBOARD_STAGE_LINK_RESTORE
};
enum giulia_wifi_board_owned {
    GIULIA_WBOARD_OWNS_GPIO = 1u << 0,
    GIULIA_WBOARD_OWNS_POWER = 1u << 1,
    GIULIA_WBOARD_OWNS_LINK = 1u << 2,
    GIULIA_WBOARD_OWNS_PHY = 1u << 3
};

struct giulia_wifi_board_phy_io {
    void *cookie;
    uint32_t (*read32)(void *, uint32_t);
    void (*write32)(void *, uint32_t, uint32_t);
    void (*barrier)(void *);
    void (*delay_us)(void *, uint32_t);
};
struct giulia_wifi_board_io {
    struct giulia_wifi_gpio_io gpio;
    struct giulia_pcie_power_io power;
    struct giulia_pcie_link_io link;
    struct giulia_wifi_board_phy_io phy;
    void *external_cookie;
    /* Return zero only while the required externally owned resources and
     * exclusive access actually remain held. Consult the owning platform
     * services, not a read-only snapshot or requested-but-uncompleted votes.
     * Called before each stage and PIPE operation. Never revoke resources
     * during an operation or until board stop succeeds. This callback must
     * not acquire/release hardware, reenter the board, or invent a lease.
     */
    int (*validate_external)(void *, uint32_t required);
};
struct giulia_wifi_board {
    struct giulia_wifi_board_io io;
    struct giulia_wifi_gpio gpio;
    struct giulia_pcie_power power;
    struct giulia_pcie_link link;
    struct giulia_pcie_phy phy;
    uint32_t state, stage, owned, conditions;
    uint32_t failed_stage, cleanup_stage;
    int error, driver_error, cleanup_error, cleanup_driver_error, external_error;
    uint8_t host_clocks_ready, perst_isolated, link_may_run;
};

/* Zero-initialize once and keep this object's address stable until STOPPED:
 * the bound PHY callbacks refer to its embedded power core. Caller serializes
 * start/stop and all CPU BAR users on one owner; no kernel locking is implied.
 * GPIO -> WLAN_EN -> clocks/reset -> PARF -> PHY/PIPE -> PERST# release ->
 * link/config -> explicit CPU MEM decoding. Success permits CPU BAR reads
 * only; BME/MSI/DMA remain disabled and this is not a usable Wi-Fi interface.
 * On failure, reverse cleanup is attempted. Inspect state/owned/cleanup_error:
 * FAULT retains all remaining mappings and external resources for retry.
 */
int giulia_wifi_board_start(struct giulia_wifi_board *,
    const struct giulia_wifi_board_io *, uint32_t conditions);
/* Upper users must be quiesced first. Assert PERST# -> link quiesce ->
 * PHY/PIPE -> final link/PARF restore -> power -> GPIO restore. PARF's PHY
 * power-down state must not be restored before QMP has stopped.
 * Stop immediately at the first failure; never turn
 * off a lower layer needed to retry an upper layer. Only STOPPED permits the
 * caller to release external supplies, NoC, CESTA ownership and mappings.
 */
int giulia_wifi_board_stop(struct giulia_wifi_board *, uint32_t conditions);

#endif
