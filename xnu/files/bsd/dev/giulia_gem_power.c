/* SPDX-License-Identifier: BSD-2-Clause */
/* Original lifecycle composed with the actual RPMh and GEM clock cores.
 * Vendor protocol: clk-rpmh.c pineapple xo_pad_ao -> xo.lvl selector3;
 * clk-regmap.c prepare -> gcc-pineapple.c vdd_cx_ao LOWER at XO frequency;
 * rpmh-regulator.c ARC selectors/max aggregation, no separate enable write.
 * References: OnePlusOSS/android_kernel_oneplus_sm8650@
 * 66305c56af1fcd7299c906c3257e585be93d93ed. No Linux code copied. */
#include "giulia_gem_power.h"
#define BIT(n) (1u<<(n))
static uintptr_t power_owner;
int giulia_gem_power_plan(const uint8_t *db,uint32_t bytes,uint32_t version,
    const struct giulia_gem_power_baseline *b,struct giulia_gem_power_plan *out)
{
    struct giulia_gem_power_plan p={0}; int rc;
    if (!out) return GIULIA_RPMH_ARGUMENT;
    *out=p;
    if (!b || b->known!=3) return GIULIA_GEM_POWER_BASELINE_UNKNOWN;
    if ((rc=giulia_arc_resolve(db,bytes,version,"xo.lvl",&p.resource[0])) ||
        (rc=giulia_arc_resolve(db,bytes,version,"cx.lvl",&p.resource[1]))) return rc;
    if (p.resource[0].address==p.resource[1].address ||
        p.resource[0].count<4 || p.resource[0].level[0]!=0 ||
        b->selector[0]>3 || b->selector[1]>=p.resource[1].count) return GIULIA_RPMH_DATABASE;
    p.target[0]=3;
    if ((rc=giulia_arc_select(&p.resource[1],GIULIA_GEM_CX_AO_MIN_LEVEL,&p.target[1]))) return rc;
    for (unsigned i=0;i<2;i++) {
        p.prior[i]=b->selector[i];
        if (p.target[i]<p.prior[i]) p.target[i]=p.prior[i];
    }
    *out=p; return 0;
}
static int own(struct giulia_gem_power *p)
{ return p->owned && __atomic_load_n(&power_owner,__ATOMIC_ACQUIRE)==(uintptr_t)p; }
static int valid(struct giulia_gem_power *p)
{ return p->lease.validate(p->lease.cookie) ? GIULIA_RPMH_PREREQUISITE:0; }
static int clock_lease(void *cookie)
{
    struct giulia_gem_power *p=cookie;
    if (!own(p) || p->held!=3 || p->pending || valid(p)) return -1;
    return p->backend.validate_lease(p->backend.cookie);
}
static int clock_read(void *cookie,uint32_t off,uint32_t *v)
{ struct giulia_gem_power *p=cookie; return p->backend.read32(p->backend.cookie,off,v); }
static int clock_write(void *cookie,uint32_t off,uint32_t v)
{ struct giulia_gem_power *p=cookie; return p->backend.write32(p->backend.cookie,off,v); }
static void clock_barrier(void *cookie)
{ struct giulia_gem_power *p=cookie; p->backend.barrier(p->backend.cookie); }
static void clock_delay(void *cookie,unsigned us)
{ struct giulia_gem_power *p=cookie; p->backend.delay_us(p->backend.cookie,us); }
static void command_done(struct giulia_gem_power *p,unsigned i,int restore)
{
    if (restore) { p->dirty&=~BIT(i); p->held&=~BIT(i); }
    else p->held|=BIT(i);
}
static int send(struct giulia_gem_power *p,unsigned i,int restore)
{
    int rc=valid(p);
    if (rc) return rc;
    struct giulia_rpmh *t=p->transport;
    if (t->state!=GIULIA_RPMH_IDLE && t->state!=GIULIA_RPMH_DONE) return GIULIA_RPMH_STATE;
    struct giulia_rpmh_command c={p->plan.resource[i].address,restore ? p->plan.prior[i]:p->plan.target[i]};
    rc=giulia_rpmh_write(t,&c,1);
    if (!restore && t->triggered) p->dirty|=BIT(i);
    p->pending=t->dirty || giulia_rpmh_bus_owned(t);
    if (p->pending) { p->pending_index=i; p->pending_restore=(uint32_t)restore; }
    if (!rc) command_done(p,i,restore);
    return rc;
}
int giulia_gem_power_stop(struct giulia_gem_power *p)
{
    int rc; uintptr_t expected;
    if (!p) return GIULIA_RPMH_ARGUMENT;
    if (p->state==GIULIA_GEM_POWER_IDLE || p->state==GIULIA_GEM_POWER_STOPPED) return 0;
    if (!own(p)) return GIULIA_RPMH_STATE;
    p->cleanup_error=0;
    if ((rc=valid(p))) goto failed;
    /* The clock never gets acquired until BOTH resource commands completed;
     * restore all clock users before touching either backing resource. */
    if ((rc=giulia_gem_clock_stop(&p->clock))) goto failed;
    if (p->pending) {
        if ((rc=giulia_rpmh_recover(p->transport))) goto failed;
        if (p->transport->completed)
            command_done(p,p->pending_index,(int)p->pending_restore);
        p->pending=0;
    }
    for (unsigned n=2;n>0;n--) {
        unsigned i=n-1;
        if (!(p->dirty&BIT(i))) continue;
        if ((rc=send(p,i,1))) goto failed;
    }
    expected=(uintptr_t)p;
    if (!__atomic_compare_exchange_n(&power_owner,&expected,0,0,__ATOMIC_RELEASE,__ATOMIC_RELAXED)) {
        rc=GIULIA_RPMH_STATE; goto failed;
    }
    p->owned=0; p->held=0; p->state=GIULIA_GEM_POWER_STOPPED; return 0;
failed:
    p->cleanup_error=rc; p->state=GIULIA_GEM_POWER_FAULT; return rc;
}
int giulia_gem_power_check(struct giulia_gem_power *p)
{
    if (!p || !own(p) || p->state!=GIULIA_GEM_POWER_ACTIVE) return GIULIA_RPMH_STATE;
    int rc=clock_lease(p);
    if (!rc) rc=giulia_gem_clock_check(&p->clock);
    return rc;
}
int giulia_gem_power_start(struct giulia_gem_power *p,struct giulia_rpmh *t,
    const struct giulia_gem_power_lease *l,const struct giulia_gem_clock_io *io,
    const uint8_t *db,uint32_t bytes,uint32_t version,const struct giulia_gem_power_baseline *b)
{
    struct giulia_gem_power_plan plan; uintptr_t expected=0; int rc;
    if (!p || !t || !l || !l->validate || !io || !io->read32 || !io->write32 ||
        !io->barrier || !io->delay_us || !io->validate_lease) return GIULIA_RPMH_ARGUMENT;
    if (p->state!=GIULIA_GEM_POWER_IDLE && p->state!=GIULIA_GEM_POWER_STOPPED) return GIULIA_RPMH_STATE;
    if (io->gcc_phys!=GIULIA_GEM_GCC_PHYS || io->gcc_size!=GIULIA_GEM_GCC_SIZE ||
        (t->prerequisites&GIULIA_RPMH_REQUIRED)!=GIULIA_RPMH_REQUIRED) return GIULIA_RPMH_PREREQUISITE;
    if (t->state!=GIULIA_RPMH_IDLE && t->state!=GIULIA_RPMH_DONE) return GIULIA_RPMH_STATE;
    if ((rc=giulia_gem_power_plan(db,bytes,version,b,&plan))) return rc;
    if (l->validate(l->cookie) || io->validate_lease(io->cookie)) return GIULIA_RPMH_PREREQUISITE;
    struct giulia_gem_power_lease lc=*l; struct giulia_gem_clock_io ic=*io;
    if (!__atomic_compare_exchange_n(&power_owner,&expected,(uintptr_t)p,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))
        return GIULIA_RPMH_BUSY;
    *p=(struct giulia_gem_power){0}; p->owned=1; p->state=GIULIA_GEM_POWER_PREPARING;
    p->transport=t; p->plan=plan; p->lease=lc; p->backend=ic;
    for (unsigned i=0;i<2;i++) {
        if (plan.target[i]==plan.prior[i]) { p->held|=BIT(i); continue; }
        if ((rc=send(p,i,0))) goto failed;
    }
    ic.cookie=p; ic.read32=clock_read; ic.write32=clock_write;
    ic.barrier=clock_barrier; ic.delay_us=clock_delay; ic.validate_lease=clock_lease;
    if ((rc=giulia_gem_clock_start(&p->clock,&ic))) goto failed;
    p->state=GIULIA_GEM_POWER_ACTIVE; return 0;
failed:
    p->error=rc; p->state=GIULIA_GEM_POWER_FAULT;
    (void)giulia_gem_power_stop(p);
    return rc;
}
