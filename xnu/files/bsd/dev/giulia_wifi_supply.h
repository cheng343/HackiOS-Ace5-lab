/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_WIFI_SUPPLY_H
#define GIULIA_WIFI_SUPPLY_H
#include "giulia_wifi_gpio.h"
#include "giulia_wlan_pdc.h"
#include "giulia_wlan_rails.h"
#include "giulia_pcie_cpu_noc.h"
#include "giulia_pcie_power.h"

enum giulia_wifi_supply_result {
    GIULIA_SUPPLY_RETAINED = 1, GIULIA_SUPPLY_ARGUMENT = -1,
    GIULIA_SUPPLY_STATE = -2, GIULIA_SUPPLY_PREREQUISITE = -3,
    GIULIA_SUPPLY_EXTERNAL = -4, GIULIA_SUPPLY_DRIVER = -5,
    GIULIA_SUPPLY_CONTRACT = -6
};
enum giulia_wifi_supply_state {
    GIULIA_SUPPLY_IDLE, GIULIA_SUPPLY_PREPARING, GIULIA_SUPPLY_HOST_PREPARED,
    GIULIA_SUPPLY_STOPPING, GIULIA_SUPPLY_STOPPED, GIULIA_SUPPLY_FAULT,
    GIULIA_SUPPLY_HELD, GIULIA_SUPPLY_HANDED_OFF
};
enum giulia_wifi_supply_stage {
    GIULIA_SUPPLY_NONE, GIULIA_SUPPLY_INPUTS, GIULIA_SUPPLY_GPIO,
    GIULIA_SUPPLY_AOP, GIULIA_SUPPLY_PDC, GIULIA_SUPPLY_RAILS,
    GIULIA_SUPPLY_CPU_NOC, GIULIA_SUPPLY_WLAN_ON, GIULIA_SUPPLY_HOST,
    GIULIA_SUPPLY_POWER_STOP, GIULIA_SUPPLY_WLAN_OFF, GIULIA_SUPPLY_CPU_STOP,
    GIULIA_SUPPLY_RAIL_STOP, GIULIA_SUPPLY_PDC_STOP, GIULIA_SUPPLY_AOP_STOP,
    GIULIA_SUPPLY_GPIO_STOP, GIULIA_SUPPLY_HOST_HANDOFF
};
enum giulia_wifi_supply_condition {
    GIULIA_SUPPLY_FIRMWARE_QUIET_HELD = 1u << 0,
    GIULIA_SUPPLY_WLAN_BT_RESOURCES_EXCLUSIVE = 1u << 1,
    GIULIA_SUPPLY_MMIO_IPCC_DRV2_OWNED = 1u << 2,
    GIULIA_SUPPLY_PCI_DMA_MSI_QUIET = 1u << 3,
    GIULIA_SUPPLY_CURRENT_DB_DT = 1u << 4,
    GIULIA_SUPPLY_UNMANAGED_MODES_READY = 1u << 5,
    GIULIA_SUPPLY_PARENT_CLOCKS_READY = 1u << 6,
    GIULIA_SUPPLY_CESTA_COORDINATED = 1u << 7,
    GIULIA_SUPPLY_NO_LOW_POWER_TRANSITION = 1u << 8,
    GIULIA_SUPPLY_CPU_BINARY_PROTOCOL_MATCHED = 1u << 9,
    GIULIA_SUPPLY_DT_MODE_WRITE_SAFE = 1u << 10,
    GIULIA_SUPPLY_RCHNG_HANDOFF = 1u << 11,
    /* Cold, stopped PCIe0 PHY/link; no inherited active PHY/config/BAR user.
     * Required before assuming PHY_STOPPED during pre-host GPIO rollback. */
    GIULIA_SUPPLY_COLD_PCIE_PHY_QUIET = 1u << 12
};
#define GIULIA_SUPPLY_REQUIRED 0x13ffu
enum giulia_wifi_supply_retained {
    GIULIA_SUPPLY_RETAINS_CPU_NOC = 1u << 0,
    GIULIA_SUPPLY_RETAINS_PDC = 1u << 1,
    GIULIA_SUPPLY_RETAINS_RAILS = 1u << 2
};
struct giulia_wifi_supply_config {
    struct giulia_wifi_gpio_io gpio;
    struct giulia_aop_io aop;
    struct giulia_rpmh_io rpmh;
    struct giulia_pcie_power_io power;
    const uint8_t *db, *pdc_table;
    uint32_t db_bytes, db_version, pdc_bytes;
    const struct giulia_wlan_rail_baseline *rail_baseline;
    const struct giulia_wlan_pdc_baseline *pdc_baseline;
    uint32_t conditions, dt_modes;
    unsigned aop_policy, pdc_policy, rail_policy;
    /* Required explicit acknowledgement of the forward-only CPU-NoC floor.
     * Strict rail baseline restoration is always used by this coordinator. */
    unsigned allow_retained_cpu_floor;
    void *owner_cookie;
    /* Consult actual platform owners at EVERY stage and CPU-NoC operation.
     * Zero means the required firmware quiet, shared supply/pin/RSC/IRQ and
     * CESTA/GCC contracts remain held, not that a bit was seen in a snapshot.
     * No resource may be revoked during a call or while a retained owner
     * exists. Do not send USB/firmware requests while firmware is parked. */
    int (*validate_external)(void *, uint32_t required, unsigned stage);
};
struct giulia_wifi_supply {
    struct giulia_wifi_supply_config config;
    struct giulia_wifi_gpio gpio;
    struct giulia_aop aop;
    struct giulia_wlan_pdc pdc;
    struct giulia_rpmh rpmh;
    struct giulia_wlan_rails rails;
    struct giulia_cpu_noc cpu_noc;
    struct giulia_pcie_power power;
    struct giulia_wlan_rail_baseline input_rails[7];
    struct giulia_wlan_pdc_baseline input_pdc;
    uint8_t input_table[508];
    uint32_t state, stage, failed_stage, cleanup_stage, retained;
    int error, driver_error, cleanup_error, cleanup_driver_error, external_error;
};
/* Zero once; keep this object, callbacks, mappings, immutable DB/DT/baseline
 * buffers and external leases stable until STOPPED or controlled cold reset.
 * Pure preflight checks all table/rail/CPU plans BEFORE GPIO/AOP writes.
 * Sequence: WLAN/BT/PERST held low -> open QMP -> PDC table -> seven strict
 * rail votes -> CPU SH1/CN0 floor -> WLAN_EN -> GCC host preparation.
 * Success is HOST_PREPARED only: no CRM data profile, BCR pulse, PHY/link,
 * BME/MSI/DMA, firmware or network startup is performed. The next CESTA/NoC
 * service must prove physical readiness before power_finish_reset; a table
 * ACK is insufficient. No automatic XNU boot hook calls this coordinator.
 */
int giulia_wifi_supply_prepare(struct giulia_wifi_supply *,
    const struct giulia_wifi_supply_config *);
/* Before the next resource owner submits any CRM/CESTA/PHY operation, mark
 * the host handoff. This does not release supply ownership or prove NoC
 * ready. It disables phase-one cancel: downstream must establish and finish
 * its own withdrawal before a later composed teardown can be implemented. */
int giulia_wifi_supply_handoff_host(struct giulia_wifi_supply *);
/* Valid only BEFORE the next CESTA/data-profile/PHY/PCIe phase is submitted.
 * Restore GCC first -> hold WLAN/reset low -> recover CPU TCS -> restore rail
 * votes -> restore/retain PDC table -> close QMP. The CPU floor, or explicit
 * KEEP_TABLE, may remain applied: HELD/RETAINED keeps GPIO reset and platform
 * ownership to cold reset, never pretends to undo shared votes. A timeout
 * stops lower teardown and keeps FAULT/resources for retry; never resends a
 * PDC/CPU message. Only fully reversible pre-publication failures can STOP.
 */
int giulia_wifi_supply_cancel(struct giulia_wifi_supply *);
#endif
