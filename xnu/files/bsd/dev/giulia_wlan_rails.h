/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_WLAN_RAILS_H
#define GIULIA_WLAN_RAILS_H
#include "giulia_rpmh.h"
#define GIULIA_WLAN_RAIL_COUNT 7u
enum giulia_wlan_rail_result {
    GIULIA_WLAN_RAIL_OK = 0,
    /* Deliberately nonzero: votes remain, no restore/power-off was claimed. */
    GIULIA_WLAN_RAIL_RETAINED = 1,
    GIULIA_WLAN_RAIL_BASELINE_UNKNOWN = -20
};
enum giulia_wlan_rail_policy {
    GIULIA_WLAN_RESTORE_BASELINE = 0, GIULIA_WLAN_KEEP_VOTES = 1
};
enum giulia_wlan_rail_state {
    GIULIA_WLAN_RAIL_IDLE = 0, GIULIA_WLAN_RAIL_ACTIVE,
    GIULIA_WLAN_RAIL_FAULT, GIULIA_WLAN_RAIL_STOPPED, GIULIA_WLAN_RAIL_HELD
};
enum giulia_wlan_rail_prerequisite {
    GIULIA_WLAN_RAIL_RESOURCES_EXCLUSIVE = 1u << 0,
    GIULIA_WLAN_RAIL_ENDPOINT_HELD_OFF = 1u << 1,
    GIULIA_WLAN_RAIL_MODE_ALREADY_READY = 1u << 2,
    GIULIA_WLAN_RAIL_DB_CURRENT_VERIFIED = 1u << 3
};
#define GIULIA_WLAN_RAIL_REQUIRED 15u
struct giulia_wlan_rail_baseline {
    uint32_t millivolts, enable;
    /* 3 means both prior ACTIVE_ONLY per-DRV vote values are known. A
     * regulator sysfs value, aggregate PMIC voltage, CMD DB resource address,
     * or saved TCS command buffer does NOT establish this baseline.
     */
    uint32_t known;
};
struct giulia_wlan_rails {
    struct giulia_rpmh *transport;
    uint32_t state, policy, attempted_mask, completed_mask;
    uint32_t address[GIULIA_WLAN_RAIL_COUNT];
    struct giulia_wlan_rail_baseline baseline[GIULIA_WLAN_RAIL_COUNT];
    int error, cleanup_error;
};
/* Order is CNSS power.c optional-regulator order filtered to this board:
 * ldob15, ldoc3, smpi4, smpc2, smpc3, smpc6, smpc1.
 * Real V/E writes at CMD DB base+0/base+4, in mV, one completed TCS per rail.
 * Mode/headroom and sleep/wake votes are untouched; MODE_ALREADY_READY must
 * be established externally, not inferred from an enabled GPIO. This is a
 * rail vote layer, not the full WLAN power sequence: AOP QMP/PDC setup, BT
 * coordination, GPIO16, pinctrl, NoC and PCIe remain with the board owner.
 * The current repository has no preboot per-DRV vote dump. Strict policy
 * therefore rejects unknown baselines before MMIO. KEEP_VOTES is an explicit
 * forward-only experiment, never selected automatically or reversible by
 * restoring TCS registers. It retains the rail ownership claim after stop.
 */
int giulia_wlan_rails_start(struct giulia_wlan_rails *, struct giulia_rpmh *,
    const uint8_t *db, uint32_t bytes, uint32_t expected_version,
    const struct giulia_wlan_rail_baseline *baseline, uint32_t prerequisites,
    enum giulia_wlan_rail_policy);
/* Requires actual endpoint/DMA/PCIe users quiesced and WLAN_EN held off.
 * Strict stop compensates all attempted writes in reverse rail order using
 * the supplied known baseline, including partial-start failures. Restore of
 * votes does not prove physical rails turn off: other masters may vote ON.
 * On any failure keep the mapping, transport, baseline and owner for retry.
 */
int giulia_wlan_rails_stop(struct giulia_wlan_rails *, int endpoint_quiesced);
#endif
