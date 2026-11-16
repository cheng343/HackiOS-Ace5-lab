/* SPDX-License-Identifier: BSD-2-Clause */
/* Original rail vote lifecycle; fixed public facts:
 * https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/vendor/qcom/proprietary/wlan-devicetree/pineapple-kiwi-cnss.dtsi
 * Seven supply names/voltages, also matched to the saved live ColorOS 17 DT.
 * https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/vendor/qcom/opensource/wlan/platform/cnss2/power.c
 * cnss_vreg_list/cnss_vreg_on_single: voltage before enable; reverse unwind.
 * https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/blob/66305c56af1fcd7299c906c3257e585be93d93ed/drivers/regulator/rpmh-regulator.c
 * VRM indices 0=millivolts, 1=enable, 2=mode, 3=headroom. get_voltage and
 * is_enabled read software cache, not reconstructable remote per-DRV votes.
 * rpmh_regulator_aggregate_requests max-aggregates each valid request field;
 * a WLAN request must not overwrite another known per-DRV voltage/mode vote.
 */
#include "giulia_wlan_rails.h"
static const char *const resource[GIULIA_WLAN_RAIL_COUNT] = {
    "ldob15", "ldoc3", "smpi4", "smpc2", "smpc3", "smpc6", "smpc1"
};
static const uint32_t millivolts[GIULIA_WLAN_RAIL_COUNT] = {
    1800, 1200, 940, 976, 916, 1864, 1316
};
/* Fixed rpmh-regulator.c PMIC5 raw encodings, not framework mode enums. */
static const uint32_t dt_mode[7] = {7,7,0,0,0,0,3};
#define DT_MODE_MASK 0x43u
static uintptr_t rail_owner;
static int own(struct giulia_wlan_rails *p)
{
    uintptr_t expected = 0;
    return __atomic_compare_exchange_n(&rail_owner, &expected, (uintptr_t)p,
        0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ? 0 : GIULIA_RPMH_BUSY;
}
static int release(struct giulia_wlan_rails *p)
{
    uintptr_t expected = (uintptr_t)p;
    return __atomic_compare_exchange_n(&rail_owner, &expected, 0,
        0, __ATOMIC_RELEASE, __ATOMIC_RELAXED) ? 0 : GIULIA_RPMH_STATE;
}
static int vote(struct giulia_wlan_rails *p, unsigned i, uint32_t mv, uint32_t enable, uint32_t mode)
{
    struct giulia_rpmh_command cmd[3];
    /* On withdrawal, remove enable before lowering its voltage vote. For an
     * enabled baseline, restore voltage before enabling. All commands wait.
     */
    unsigned count = (p->mode_mask & (1u << i)) ? 3 : 2;
    unsigned voltage = enable ? 0 : 1;
    unsigned en = enable ? count - 1 : 0;
    cmd[voltage].address = p->address[i]; cmd[voltage].data = mv;
    cmd[en].address = p->address[i] + 4; cmd[en].data = enable;
    if (count == 3) {
        unsigned m = enable ? 1 : 2;
        cmd[m].address = p->address[i] + 8; cmd[m].data = mode;
    }
    return giulia_rpmh_write(p->transport, cmd, count);
}
int giulia_wlan_rails_stop(struct giulia_wlan_rails *p, int endpoint_quiesced)
{
    int r;
    unsigned i;
    if (!p) return GIULIA_RPMH_ARGUMENT;
    if (p->state == GIULIA_WLAN_RAIL_IDLE || p->state == GIULIA_WLAN_RAIL_STOPPED)
        return 0;
    if (!endpoint_quiesced) return GIULIA_RPMH_PREREQUISITE;
    if (__atomic_load_n(&rail_owner, __ATOMIC_ACQUIRE) != (uintptr_t)p)
        return GIULIA_RPMH_STATE;
    if (p->state == GIULIA_WLAN_RAIL_HELD) return GIULIA_WLAN_RAIL_RETAINED;
    if ((r = giulia_rpmh_recover(p->transport))) goto failed;
    if (p->policy == GIULIA_WLAN_KEEP_VOTES && p->attempted_mask) {
        p->state = GIULIA_WLAN_RAIL_HELD;
        return GIULIA_WLAN_RAIL_RETAINED;
    }
    for (i = GIULIA_WLAN_RAIL_COUNT; i > 0; i--) {
        unsigned index = i - 1;
        if (!(p->attempted_mask & (1u << index))) continue;
        if ((r = vote(p, index, p->baseline[index].millivolts,
            p->baseline[index].enable, (p->mode_mask & (1u << index)) ?
            p->baseline[index].mode : 0))) goto failed;
        p->attempted_mask &= ~(1u << index);
    }
    if ((r = release(p))) goto failed;
    p->state = GIULIA_WLAN_RAIL_STOPPED; p->cleanup_error = 0;
    return 0;
failed:
    p->state = GIULIA_WLAN_RAIL_FAULT; p->cleanup_error = r;
    return r;
}
static int plan(const uint8_t *db, uint32_t bytes, uint32_t expected_version,
    const struct giulia_wlan_rail_baseline *baseline, uint32_t prerequisites,
    enum giulia_wlan_rail_policy policy, uint32_t mode_mask, uint32_t address[7])
{
    unsigned i, j;
    int r;
    if (policy != GIULIA_WLAN_RESTORE_BASELINE &&
        policy != GIULIA_WLAN_KEEP_VOTES) return GIULIA_RPMH_ARGUMENT;
    if ((prerequisites & GIULIA_WLAN_RAIL_REQUIRED) != GIULIA_WLAN_RAIL_REQUIRED)
        return GIULIA_RPMH_PREREQUISITE;
    if (mode_mask && !(prerequisites & GIULIA_WLAN_RAIL_MODE_WRITE_SAFE))
        return GIULIA_RPMH_PREREQUISITE;
    if (policy == GIULIA_WLAN_RESTORE_BASELINE) {
        if (!baseline) return GIULIA_WLAN_RAIL_BASELINE_UNKNOWN;
        for (i = 0; i < GIULIA_WLAN_RAIL_COUNT; i++)
            if ((baseline[i].known & 3u) != 3u || baseline[i].enable > 1 ||
                /* Official VRM voltage range: 0..8191000 microvolts. */
                baseline[i].millivolts > 8191u)
                return GIULIA_WLAN_RAIL_BASELINE_UNKNOWN;
        for (i = 0; i < GIULIA_WLAN_RAIL_COUNT; i++) {
            if (!(mode_mask & (1u << i))) continue;
            if (!(baseline[i].known & 4u) ||
                (baseline[i].mode != 3 && baseline[i].mode != 7 &&
                 !(i < 2 && baseline[i].mode == 4)))
                return GIULIA_WLAN_RAIL_BASELINE_UNKNOWN;
        }
        for (i = 0; i < GIULIA_WLAN_RAIL_COUNT; i++)
            if (baseline[i].millivolts > millivolts[i])
                return GIULIA_WLAN_RAIL_VOLTAGE_CONFLICT;
    }
    for (i = 0; i < GIULIA_WLAN_RAIL_COUNT; i++) {
        r = giulia_rpmh_resolve_vrm(db, bytes, expected_version, resource[i], &address[i]);
        if (r) return r;
        for (j = 0; j < i; j++)
            if (address[j] == address[i]) return GIULIA_RPMH_DATABASE;
    }
    return 0;
}
int giulia_wlan_rails_check(const uint8_t *db, uint32_t bytes, uint32_t version,
    const struct giulia_wlan_rail_baseline *baseline, uint32_t flags,
    enum giulia_wlan_rail_policy policy, unsigned dt_modes)
{
    uint32_t address[7];
    if (dt_modes > 1) return GIULIA_RPMH_ARGUMENT;
    return plan(db, bytes, version, baseline, flags, policy, dt_modes ? DT_MODE_MASK : 0, address);
}
static int start(struct giulia_wlan_rails *p, struct giulia_rpmh *transport,
    const uint8_t *db, uint32_t bytes, uint32_t expected_version,
    const struct giulia_wlan_rail_baseline *baseline, uint32_t prerequisites,
    enum giulia_wlan_rail_policy policy, uint32_t mode_mask)
{
    unsigned i;
    int r;
    if (!p || !transport) return GIULIA_RPMH_ARGUMENT;
    if (p->state != GIULIA_WLAN_RAIL_IDLE && p->state != GIULIA_WLAN_RAIL_STOPPED)
        return GIULIA_RPMH_STATE;
    if ((transport->prerequisites & GIULIA_RPMH_REQUIRED) != GIULIA_RPMH_REQUIRED)
        return GIULIA_RPMH_PREREQUISITE;
    if (transport->state != GIULIA_RPMH_IDLE && transport->state != GIULIA_RPMH_DONE)
        return GIULIA_RPMH_STATE;
    if ((r = plan(db, bytes, expected_version, baseline, prerequisites, policy, mode_mask, p->address))) return r;
    if ((r = own(p))) return r;
    p->transport = transport; p->policy = policy;
    p->mode_mask = mode_mask;
    for (i = 0; i < GIULIA_WLAN_RAIL_COUNT; i++) {
        p->selected_mode[i] = dt_mode[i];
        if (policy == GIULIA_WLAN_RESTORE_BASELINE && (mode_mask & (1u << i)) &&
            baseline[i].mode > p->selected_mode[i])
            p->selected_mode[i] = baseline[i].mode;
    }
    p->attempted_mask = p->completed_mask = 0;
    p->error = p->cleanup_error = 0; p->state = GIULIA_WLAN_RAIL_FAULT;
    if (policy == GIULIA_WLAN_RESTORE_BASELINE)
        for (i = 0; i < GIULIA_WLAN_RAIL_COUNT; i++) p->baseline[i] = baseline[i];
    for (i = 0; i < GIULIA_WLAN_RAIL_COUNT; i++) {
        p->attempted_mask |= 1u << i; /* May have issued even if cleanup fails. */
        r = vote(p, i, millivolts[i], 1, p->selected_mode[i]);
        if (transport->completed) p->completed_mask |= 1u << i;
        if (r) {
            if (!transport->triggered) p->attempted_mask &= ~(1u << i);
            p->error = r;
            /* No GPIO/PCIe user was enabled by this operation. Stop handles
             * any in-flight TCS before compensation; retains faults on timeout.
             */
            p->cleanup_error = giulia_wlan_rails_stop(p, 1);
            return r;
        }
    }
    p->state = GIULIA_WLAN_RAIL_ACTIVE;
    return 0;
}
int giulia_wlan_rails_start(struct giulia_wlan_rails *p, struct giulia_rpmh *transport,
    const uint8_t *db, uint32_t bytes, uint32_t version,
    const struct giulia_wlan_rail_baseline *baseline, uint32_t flags,
    enum giulia_wlan_rail_policy policy)
{ return start(p,transport,db,bytes,version,baseline,flags,policy,0); }
int giulia_wlan_rails_start_with_dt_modes(struct giulia_wlan_rails *p, struct giulia_rpmh *transport,
    const uint8_t *db, uint32_t bytes, uint32_t version,
    const struct giulia_wlan_rail_baseline *baseline, uint32_t flags,
    enum giulia_wlan_rail_policy policy)
{ return start(p,transport,db,bytes,version,baseline,flags,policy,DT_MODE_MASK); }
