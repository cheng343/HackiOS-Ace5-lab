/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_GEM_POWER_H
#define GIULIA_GEM_POWER_H
#include "giulia_arc.h"
#include "giulia_gem_clock.h"
enum giulia_gem_power_state {
    GIULIA_GEM_POWER_IDLE, GIULIA_GEM_POWER_PREPARING, GIULIA_GEM_POWER_ACTIVE,
    GIULIA_GEM_POWER_FAULT, GIULIA_GEM_POWER_STOPPED
};
enum giulia_gem_power_result { GIULIA_GEM_POWER_BASELINE_UNKNOWN=-20 };
/* XO first, then CX. Values are prior ACTIVE_ONLY per-DRV hardware selectors,
 * NOT regulator sysfs, aggregate rail state or last TCS buffer contents.
 * The same DRV's normal and AO consumers must already be aggregated. */
struct giulia_gem_power_baseline { uint32_t selector[2], known; };
struct giulia_gem_power_plan {
    struct giulia_arc_resource resource[2];
    uint32_t prior[2], target[2];
};
struct giulia_gem_power_lease {
    void *cookie;
    /* Current CMD DB, exclusive per-DRV XO/CX resource ownership + stable
     * verified baseline, all GCC/shared users quiet, firmware coordinated,
     * no low-power entry, DRV2/IRQ guarded, safe baseline restoration.
     * This lifetime is held through STOPPED, including any pending command.
     * Callback consults actual owners, never a cached flag/success stub. */
    int (*validate)(void *);
};
struct giulia_gem_power {
    struct giulia_gem_power_plan plan;
    struct giulia_gem_power_lease lease;
    struct giulia_gem_clock_io backend;
    struct giulia_gem_clock clock;
    struct giulia_rpmh *transport;
    uint32_t state,owned,dirty,held,pending,pending_index,pending_restore;
    int error,cleanup_error;
};
/* Pure planning. XO uses the vendor pineapple xo_pad_ao ON selector3;
 * CX minimum64 is looked up in CMD DB, never sent as the payload.
 * Max-aggregate known prior selectors; no unknown-baseline/forward-only mode. */
int giulia_gem_power_plan(const uint8_t *,uint32_t bytes,uint32_t version,
    const struct giulia_gem_power_baseline *,struct giulia_gem_power_plan *);
/* Actual RPMh XO -> CX completion, then actual GEM XO clock acquisition.
 * ACTIVE_ONLY: does not program sleep/wake votes or permit low-power entry.
 * No Wi-Fi boot hook, NoC proxy/QoS/PHY/PCIe or radio initialization. */
int giulia_gem_power_start(struct giulia_gem_power *,struct giulia_rpmh *,
    const struct giulia_gem_power_lease *,const struct giulia_gem_clock_io *,
    const uint8_t *,uint32_t bytes,uint32_t version,const struct giulia_gem_power_baseline *);
/* Read-only prerequisite for a real QoS owner. Does not prove full NoC ready. */
int giulia_gem_power_check(struct giulia_gem_power *);
/* Caller must finish/restore all QoS clients BEFORE stop. Clock is restored
 * before CX, then XO. Stop retries observe an in-flight RPMh transaction;
 * never resubmit it, never lower an unresolved vote or discard its owner.
 * Keep object, mapping, baseline and all leases at stable addresses to STOPPED. */
int giulia_gem_power_stop(struct giulia_gem_power *);
#endif
