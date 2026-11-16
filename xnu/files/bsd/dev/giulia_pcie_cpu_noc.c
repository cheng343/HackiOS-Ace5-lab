/* SPDX-License-Identifier: BSD-2-Clause */
/* Original CPU-path floor lifecycle. Fixed OnePlus kernel commit
 * 66305c56af1fcd7299c906c3257e585be93d93ed:
 * drivers/interconnect/qcom/pineapple.c: chm_apps -> qns_pcie ->
 * qnm_gemnoc_pcie -> xs_pcie_0, and CPU configuration via qsm_cfg/qhs_pcie0_cfg.
 * bcm_sh1/bcm_cn0 are HLOS enable_mask=1; CN0 is a keepalive resource.
 * bcm-voter.c:bcm_aggregate_mask uses X=0 and ORs Y's enable bit, not numeric
 * unit scaling. The data path in dedicated PCIe CRM HW DRV0 is separate.
 * No Linux code is copied; no firmware/DT dump or physical address is embedded.
 */
#include "giulia_pcie_cpu_noc.h"
static uintptr_t cpu_path_owner;

int giulia_cpu_noc_plan(const uint8_t *db, uint32_t bytes, uint32_t version,
    struct giulia_cpu_noc_plan *out)
{
    struct giulia_cpu_noc_plan p = {0};
    static const char *const name[2] = {"SH1", "CN0"};
    int rc;
    if (!out) return GIULIA_BCM_ARGUMENT;
    *out = p;
    for (unsigned i = 0; i < 2; i++) {
        if ((rc = giulia_bcm_resolve(db, bytes, version, name[i], &p.resource[i])) ||
            (rc = giulia_bcm_encode(&p.resource[i], 0, 1, 1, 1, 1, &p.vote[i]))) return rc;
    }
    if (p.resource[0].address == p.resource[1].address) return GIULIA_BCM_DATABASE;
    unsigned first = p.resource[0].vcd <= p.resource[1].vcd ? 0 : 1;
    for (unsigned i = 0; i < 2; i++) {
        unsigned n = i ? 1 - first : first;
        p.command[i] = (struct giulia_rpmh_command){p.resource[n].address, p.vote[n].command};
    }
    *out = p;
    return 0;
}
static int owns(struct giulia_cpu_noc *p)
{ return __atomic_load_n(&cpu_path_owner, __ATOMIC_ACQUIRE) == (uintptr_t)p; }
static int release(struct giulia_cpu_noc *p)
{
    uintptr_t expected = (uintptr_t)p;
    if (!__atomic_compare_exchange_n(&cpu_path_owner, &expected, 0, 0,
        __ATOMIC_RELEASE, __ATOMIC_RELAXED)) return GIULIA_RPMH_STATE;
    p->state = GIULIA_CPU_NOC_STOPPED;
    return 0;
}
int giulia_cpu_noc_stop_keep(struct giulia_cpu_noc *p)
{
    int rc;
    if (!p) return GIULIA_RPMH_ARGUMENT;
    if (p->state == GIULIA_CPU_NOC_IDLE || p->state == GIULIA_CPU_NOC_STOPPED) return 0;
    if (!owns(p)) return GIULIA_RPMH_STATE;
    if (p->state == GIULIA_CPU_NOC_HELD) return GIULIA_CPU_NOC_RETAINED;
    if (p->lease.validate(p->lease.cookie)) return p->cleanup_error = GIULIA_RPMH_PREREQUISITE;
    /* A successfully recovered ACTIVE transaction no longer owns the RSC
     * bus. Do not recover an unrelated later use of the same transport. */
    if (p->state == GIULIA_CPU_NOC_ACTIVE) {
        p->state = GIULIA_CPU_NOC_HELD; p->cleanup_error = 0;
        return GIULIA_CPU_NOC_RETAINED;
    }
    rc = giulia_rpmh_recover(p->transport);
    p->completed = p->transport->completed;
    if (rc) { p->state = GIULIA_CPU_NOC_FAULT; return p->cleanup_error = rc; }
    p->cleanup_error = 0;
    if (p->triggered) { p->state = GIULIA_CPU_NOC_HELD; return GIULIA_CPU_NOC_RETAINED; }
    return release(p);
}
int giulia_cpu_noc_start_keep(struct giulia_cpu_noc *p, struct giulia_rpmh *transport,
    const struct giulia_cpu_noc_lease *lease, const uint8_t *db, uint32_t bytes,
    uint32_t version, uint32_t flags)
{
    struct giulia_cpu_noc_plan plan;
    uintptr_t expected = 0;
    int rc;
    if (!p || !transport || !lease || !lease->validate) return GIULIA_RPMH_ARGUMENT;
    if (p->state != GIULIA_CPU_NOC_IDLE && p->state != GIULIA_CPU_NOC_STOPPED) return GIULIA_RPMH_STATE;
    if ((flags & GIULIA_CPU_NOC_REQUIRED) != GIULIA_CPU_NOC_REQUIRED ||
        (transport->prerequisites & GIULIA_RPMH_REQUIRED) != GIULIA_RPMH_REQUIRED)
        return GIULIA_RPMH_PREREQUISITE;
    if (transport->state != GIULIA_RPMH_IDLE && transport->state != GIULIA_RPMH_DONE) return GIULIA_RPMH_STATE;
    if ((rc = giulia_cpu_noc_plan(db, bytes, version, &plan))) return rc;
    if (lease->validate(lease->cookie)) return GIULIA_RPMH_PREREQUISITE;
    if (!__atomic_compare_exchange_n(&cpu_path_owner, &expected, (uintptr_t)p, 0,
        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return GIULIA_RPMH_BUSY;
    p->transport = transport; p->plan = plan; p->lease = *lease;
    p->state = GIULIA_CPU_NOC_FAULT; p->triggered = p->completed = 0;
    p->error = p->cleanup_error = 0;
    if (lease->validate(lease->cookie)) { release(p); return p->error = GIULIA_RPMH_PREREQUISITE; }
    rc = giulia_rpmh_write(transport, plan.command, 2);
    p->triggered = transport->triggered; p->completed = transport->completed;
    if (rc) {
        p->error = rc;
        /* A dirty but not triggered TCS still needs its original owner for
         * cleanup. Never hide it by releasing only this higher layer. */
        if (!p->triggered && !transport->dirty && !giulia_rpmh_bus_owned(transport)) release(p);
        return rc;
    }
    p->state = GIULIA_CPU_NOC_ACTIVE;
    return 0;
}
