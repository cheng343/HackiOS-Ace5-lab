/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_WLAN_RAILS_H
#define GIULIA_WLAN_RAILS_H
#include "giulia_rpmh.h"
#define GIULIA_WLAN_RAIL_COUNT 7u
enum giulia_wlan_rail_result {
    GIULIA_WLAN_RAIL_OK = 0,
    /* Deliberately nonzero: votes remain, no restore/power-off was claimed. */
    GIULIA_WLAN_RAIL_RETAINED = 1,
    GIULIA_WLAN_RAIL_BASELINE_UNKNOWN = -20,
    GIULIA_WLAN_RAIL_VOLTAGE_CONFLICT = -21
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
    GIULIA_WLAN_RAIL_DB_CURRENT_VERIFIED = 1u << 3,
    /* For the mode-setting API only: the affected physical rails are off,
     * or their owning service has proved the target hardware mode already
     * applies. A prior per-DRV enable=0 alone does not prove a physical OFF. */
    GIULIA_WLAN_RAIL_MODE_WRITE_SAFE = 1u << 4
};
#define GIULIA_WLAN_RAIL_REQUIRED 15u
struct giulia_wlan_rail_baseline {
    uint32_t millivolts, enable;
    /* Bits 0/1 establish prior ACTIVE_ONLY per-DRV voltage/enable votes. A
     * regulator sysfs value, aggregate PMIC voltage, CMD DB resource address,
     * or saved TCS command buffer does NOT establish this baseline.
     */
    uint32_t known;
    /* known bit2 additionally establishes the previous per-DRV raw mode.
     * PMIC5 LDO permits 3/4/7; this board's S1C FTSMPS permits 3/7. */
    uint32_t mode;
};
struct giulia_wlan_rails {
    struct giulia_rpmh *transport;
    uint32_t state, policy, attempted_mask, completed_mask;
    uint32_t mode_mask;
    uint32_t selected_mode[GIULIA_WLAN_RAIL_COUNT];
    uint32_t address[GIULIA_WLAN_RAIL_COUNT];
    struct giulia_wlan_rail_baseline baseline[GIULIA_WLAN_RAIL_COUNT];
    int error, cleanup_error;
};
/* Pure validation for a composed startup before it sends any AOP message or
 * changes GPIO: current CMD DB, strict baseline, modes and voltage conflicts.
 * dt_modes is exactly0/1. No RSC lease, resource claim or MMIO is acquired. */
int giulia_wlan_rails_check(const uint8_t *, uint32_t bytes, uint32_t version,
    const struct giulia_wlan_rail_baseline *, uint32_t prerequisites,
    enum giulia_wlan_rail_policy, unsigned dt_modes);
/* Order is CNSS power.c optional-regulator order filtered to this board:
 * ldob15, ldoc3, smpi4, smpc2, smpc3, smpc6, smpc1.
 * Real V/E writes at CMD DB base+0/base+4, in mV, one completed TCS per rail.
 * This legacy API leaves modes, headroom and sleep/wake votes untouched;
 * MODE_ALREADY_READY must
 * be established externally, not inferred from an enabled GPIO. This is a
 * rail vote layer, not the full WLAN power sequence: AOP QMP/PDC setup, BT
 * coordination, GPIO16, pinctrl, NoC and PCIe remain with the board owner.
 * The current repository has no preboot per-DRV vote dump. Strict policy
 * therefore rejects unknown baselines before MMIO. KEEP_VOTES is an explicit
 * forward-only experiment, never selected automatically or reversible by
 * restoring TCS registers. It retains the rail ownership claim after stop.
 * Strict start also rejects a prior per-DRV voltage above this WLAN
 * consumer's exact min=max voltage: lowering it loses a shared request, but
 * raising the WLAN target violates its supplied limit. Resolve that conflict
 * in the owning regulator service, never silently replace the prior vote.
 */
int giulia_wlan_rails_start(struct giulia_wlan_rails *, struct giulia_rpmh *,
    const uint8_t *db, uint32_t bytes, uint32_t expected_version,
    const struct giulia_wlan_rail_baseline *baseline, uint32_t prerequisites,
    enum giulia_wlan_rail_policy);
/* Board-specific optional profile, matching the verified live DT initial
 * modes: L15B/L3C PMIC5 LDO HPM=7, S1C PMIC5 FTSMPS RET=3. The other four
 * mode fields are not written; MODE_ALREADY_READY covers these unmanaged
 * modes. MODE_WRITE_SAFE is additionally required. Strict restoration needs
 * known prior mode votes for all three fields; KEEP_VOTES never claims undo.
 * Strict start max-aggregates each managed mode with its known per-DRV vote,
 * matching rpmh-regulator.c: a prior S1C HPM=7 is not lowered to DT RET=3.
 * MODE_WRITE_SAFE must cover the selected, aggregated modes.
 * Enabled sequence is V -> M -> E; withdrawal is E -> V -> M.
 */
int giulia_wlan_rails_start_with_dt_modes(struct giulia_wlan_rails *, struct giulia_rpmh *,
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
