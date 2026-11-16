/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_PCIE_CPU_NOC_H
#define GIULIA_PCIE_CPU_NOC_H
#include "giulia_bcm.h"
#include "giulia_rpmh.h"

enum giulia_cpu_noc_result { GIULIA_CPU_NOC_RETAINED = 1 };
enum giulia_cpu_noc_state {
    GIULIA_CPU_NOC_IDLE, GIULIA_CPU_NOC_ACTIVE, GIULIA_CPU_NOC_FAULT,
    GIULIA_CPU_NOC_STOPPED, GIULIA_CPU_NOC_HELD
};
enum giulia_cpu_noc_condition {
    GIULIA_CPU_NOC_HLOS_VOTES_EXCLUSIVE = 1u << 0,
    GIULIA_CPU_NOC_CURRENT_DB = 1u << 1,
    /* Current board description matches HLOS CN0/SH1 enable_mask=1,
     * perf_mode_mask=0. CMD DB alone does not establish these semantics. */
    GIULIA_CPU_NOC_BINARY_PROTOCOL_MATCHED = 1u << 2,
    GIULIA_CPU_NOC_ALL_CLIENTS_SERIALIZED = 1u << 3,
    /* This is ACTIVE_ONLY; it does not install wake/sleep votes. */
    GIULIA_CPU_NOC_NO_LOW_POWER_TRANSITION = 1u << 4
};
#define GIULIA_CPU_NOC_REQUIRED 31u
struct giulia_cpu_noc_plan {
    /* SH1 then CN0 in resource[]; commands are sorted by VCD. Each has
     * VALID+COMMIT and its own waited completion, even for an equal VCD. */
    struct giulia_bcm_resource resource[2];
    struct giulia_bcm_vote vote[2];
    struct giulia_rpmh_command command[2];
};
struct giulia_cpu_noc_lease {
    void *cookie;
    int (*validate)(void *);
};
struct giulia_cpu_noc {
    struct giulia_rpmh *transport;
    struct giulia_cpu_noc_plan plan;
    struct giulia_cpu_noc_lease lease;
    uint32_t state, triggered, completed;
    int error, cleanup_error;
};
/* Pure current-CMD-DB planning for the HLOS CPU->PCIe path. Binary floors
 * are X=0/Y=1; no voltage, RCG, CRM HW-table or arbitrary bandwidth write. */
int giulia_cpu_noc_plan(const uint8_t *, uint32_t bytes, uint32_t version,
    struct giulia_cpu_noc_plan *);
/* Explicit forward-only floor: there is no prior per-DRV BCM vote read API.
 * No zero/off or invented restoration is issued. The real platform owner
 * must retain exclusive leases, mappings and the no-low-power condition
 * through cold reset after any possibly issued command. Validate consults
 * that owner, never merely a saved flag or an unchanged register snapshot.
 * The RPMh backend separately enforces DRV2/TCS/IRQ ownership and completion.
 * ACTIVE means both commands completed; NOT proof of physical NoC/QoS ready
 * or permission to release PCIe reset. No current boot hook calls this API.
 */
int giulia_cpu_noc_start_keep(struct giulia_cpu_noc *, struct giulia_rpmh *,
    const struct giulia_cpu_noc_lease *, const uint8_t *, uint32_t bytes,
    uint32_t version, uint32_t conditions);
/* Observe only, never resubmit. After an issued transaction completes and
 * its local TCS programming is recovered, returns RETAINED with the CPU path
 * floor still owned until cold reset. Pending/error retains the RSC too. */
int giulia_cpu_noc_stop_keep(struct giulia_cpu_noc *);
#endif
