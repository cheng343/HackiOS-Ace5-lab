/* SPDX-License-Identifier: BSD-2-Clause */
/* Cloud-only production clock + production QoS composition. Parent/voltage
 * ownership is a fixture; these tests cannot establish device readiness. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "giulia_gem_clock.h"
#include "giulia_pcie_qos.h"
#define BIT(n) (1u<<(n))
#define FORCE BIT(1)
#define OFF BIT(31)
struct fake {
    uint32_t cmd,cfg,initial_cfg,q[3];
    unsigned reads,writes,leases,delays,barriers,fail_read,fail_write,fail_lease;
    unsigned ones_read,drop_write,committed_error,persistent,stuck_off;
    unsigned ready_after,q_reads,q_writes,q_fail_write;
    int parent_voltage_ok;
    struct giulia_gem_clock *clock;
};
static int rd(void *cookie,uint32_t off,uint32_t *v)
{
    struct fake *f=cookie;
    assert(off==0x63004 || off==0x63008); assert(++f->reads<3000);
    if (f->reads==f->fail_read) return -81;
    if (off==0x63004 && (f->cmd&FORCE) && !f->stuck_off && f->delays>=f->ready_after)
        f->cmd&=~OFF;
    *v=f->reads==f->ones_read ? UINT32_MAX : off==0x63004 ? f->cmd : f->cfg;
    return 0;
}
static int wr(void *cookie,uint32_t off,uint32_t v)
{
    struct fake *f=cookie;
    assert(off==0x63004 && ((f->cmd^v)&~FORCE)==0);
    assert(!(v&0xf1)); /* Never send UPDATE or dirty bits. */
    assert(++f->writes<30);
    if (f->writes==f->drop_write) return 0;
    if (f->writes==f->fail_write && !f->committed_error) return -82;
    f->cmd=v;
    return f->writes==f->fail_write ? -82 : 0;
}
static void barrier(void *cookie) { ((struct fake *)cookie)->barriers++; }
static void delay(void *cookie,unsigned us)
{ assert(us==1); ((struct fake *)cookie)->delays++; }
static int lease(void *cookie)
{
    struct fake *f=cookie; f->leases++;
    return !f->parent_voltage_ok || (f->fail_lease && (f->leases==f->fail_lease ||
        (f->persistent && f->leases>f->fail_lease))) ? -83 : 0;
}
static struct giulia_gem_clock_io init(struct fake *f,struct giulia_gem_clock *p)
{
    memset(f,0,sizeof(*f)); memset(p,0,sizeof(*p));
    f->cmd=OFF|BIT(12); f->cfg=f->initial_cfg=0x00100001;
    f->parent_voltage_ok=1; f->clock=p;
    return (struct giulia_gem_clock_io){.cookie=f,.read32=rd,.write32=wr,.barrier=barrier,
        .delay_us=delay,.validate_lease=lease,.gcc_phys=0x100000,.gcc_size=0x1f4200};
}
static void clear_failures(struct fake *f)
{
    f->fail_read=f->fail_write=f->fail_lease=f->ones_read=f->drop_write=f->persistent=0;
    f->parent_voltage_ok=1;
}
static void stopped(struct fake *f,struct giulia_gem_clock *p,uint32_t force)
{
    assert(p->state==GIULIA_GEM_STOPPED && !p->owned && !p->dirty);
    assert((f->cmd&FORCE)==force && (f->cmd&BIT(12)) && f->cfg==f->initial_cfg);
    assert(f->barriers==f->writes);
}
static void admission(void)
{
    struct fake f; struct giulia_gem_clock p;
    struct giulia_gem_clock_io io=init(&f,&p),bad=io;
    bad.gcc_phys+=4; assert(giulia_gem_clock_start(&p,&bad)==GIULIA_GEM_LAYOUT);
    bad=io; bad.gcc_size-=4; assert(giulia_gem_clock_start(&p,&bad)==GIULIA_GEM_LAYOUT);
    bad=io; bad.validate_lease=0; assert(giulia_gem_clock_start(&p,&bad)==GIULIA_GEM_ARGUMENT);
    f.parent_voltage_ok=0; assert(giulia_gem_clock_start(&p,&io)==GIULIA_GEM_LEASE);
    assert(!f.reads && !f.writes);
    f.parent_voltage_ok=1;
    for (unsigned b=0;b<8;b++) {
        if (b>0 && b<4) continue;
        f.cmd|=BIT(b); assert(giulia_gem_clock_start(&p,&io)==GIULIA_GEM_BUSY);
        assert(!f.writes); f.cmd&=~BIT(b);
    }
    const uint32_t badcfg[]={0x00100601,0x00100003,0x00101001,0x00000001,
        0x00100600,0x00101000,0x00000000,UINT32_MAX};
    for (unsigned i=0;i<sizeof(badcfg)/sizeof(badcfg[0]);i++) {
        f.cfg=badcfg[i]; assert(giulia_gem_clock_start(&p,&io)<0); assert(!f.writes);
    }
    f.cfg=0x00100001;
    assert(!giulia_gem_clock_start(&p,&io));
    struct giulia_gem_clock second={0},copy=p;
    unsigned w=f.writes,r=f.reads;
    assert(giulia_gem_clock_start(&second,&io)==GIULIA_GEM_STATE);
    assert(giulia_gem_clock_stop(&copy)==GIULIA_GEM_STATE);
    assert(f.writes==w && f.reads==r);
    assert(!giulia_gem_clock_stop(&p)); stopped(&f,&p,0);
    puts("GEM clock admission/pending UPDATE/unsupported parent: PASS");
}
static void success(void)
{
    struct fake f; struct giulia_gem_clock p;
    for (unsigned inherited=0;inherited<2;inherited++) {
        struct giulia_gem_clock_io io=init(&f,&p); f.cmd|=inherited ? FORCE:0;
        f.ready_after=7;
        assert(!giulia_gem_clock_start(&p,&io)); assert(f.delays==7);
        assert(!giulia_gem_clock_check(&p));
        f.cmd|=BIT(13); /* unrelated command bits survive cleanup */
        assert(!giulia_gem_clock_stop(&p)); assert(f.cmd&BIT(13));
        stopped(&f,&p,inherited ? FORCE:0); assert(f.writes==(inherited ? 0u:2u));
    }
    struct giulia_gem_clock_io io=init(&f,&p); f.stuck_off=1;
    assert(giulia_gem_clock_start(&p,&io)==GIULIA_GEM_TIMEOUT);
    assert(f.delays==500); stopped(&f,&p,0);
    puts("GEM XO force lease/borrowed vote/bounded hardware timeout: PASS");
}
static void inherited_xo(void)
{
    struct fake f; struct giulia_gem_clock p;
    for (unsigned hid=0;hid<32;hid++) for (unsigned inherited=0;inherited<2;inherited++) {
        struct giulia_gem_clock_io io=init(&f,&p);
        f.cfg=f.initial_cfg=0x100000u|hid;
        f.cmd|=inherited ? FORCE:0;
        int rc=giulia_gem_clock_start(&p,&io);
        if (hid>1) {
            assert(rc==GIULIA_GEM_CONFIG && !f.writes);
            assert(p.state==GIULIA_GEM_STOPPED && !p.owned);
            continue;
        }
        assert(!rc && p.saved_cfg==f.initial_cfg);
        assert(!giulia_gem_clock_check(&p));
        unsigned writes=f.writes;
        /* Equal rates do not grant permission to ignore a foreign writer. */
        f.cfg^=1u;
        assert(giulia_gem_clock_check(&p)==GIULIA_GEM_CONFIG);
        assert(p.state==GIULIA_GEM_FAULT && p.owned && f.writes==writes);
        if (!inherited) {
            assert(giulia_gem_clock_stop(&p)==GIULIA_GEM_CONFIG);
            assert(p.owned && p.dirty && f.writes==writes);
        }
        f.cfg=f.initial_cfg; /* External owner restores the exact baseline. */
        assert(!giulia_gem_clock_check(&p));
        assert(!giulia_gem_clock_stop(&p));
        stopped(&f,&p,inherited ? FORCE:0);
        assert(f.writes==(inherited ? 0u:2u));
    }
    puts("GEM inherited XO HID0/HID1 preserved; divided rates and equivalent-rate drift rejected: PASS");
}
static void fault_matrix(void)
{
    struct fake f; struct giulia_gem_clock p;
    struct giulia_gem_clock_io io=init(&f,&p);
    assert(!giulia_gem_clock_start(&p,&io));
    unsigned reads=f.reads,leases=f.leases,writes=f.writes;
    assert(!giulia_gem_clock_stop(&p));
    for (unsigned mode=0;mode<6;mode++) {
        unsigned count=(mode==0 || mode==1) ? reads : (mode==2 || mode==3) ? leases:writes;
        for (unsigned n=1;n<=count;n++) {
            io=init(&f,&p);
            if (mode==0) f.fail_read=n;
            if (mode==1) f.ones_read=n;
            if (mode==2 || mode==3) { f.fail_lease=n; f.persistent=mode==3; }
            if (mode==4) { f.fail_write=n; f.committed_error=1; }
            if (mode==5) f.drop_write=n;
            assert(giulia_gem_clock_start(&p,&io)<0);
            clear_failures(&f); assert(!giulia_gem_clock_stop(&p));
            if (p.state==GIULIA_GEM_IDLE) assert(!f.writes); else stopped(&f,&p,0);
        }
    }
    for (unsigned mode=0;mode<5;mode++) {
        io=init(&f,&p); assert(!giulia_gem_clock_start(&p,&io));
        if (mode==0) f.cfg^=BIT(8);
        if (mode==1) f.cmd|=BIT(0);
        if (mode==2) f.parent_voltage_ok=0;
        if (mode==3) f.fail_write=f.writes+1;
        if (mode==4) { f.fail_write=f.writes+1; f.committed_error=1; }
        assert(giulia_gem_clock_stop(&p)<0);
        assert(p.state==GIULIA_GEM_FAULT && p.owned && p.dirty);
        f.cfg=0x00100001; f.cmd&=~BIT(0); clear_failures(&f);
        assert(!giulia_gem_clock_stop(&p)); stopped(&f,&p,0);
    }
    puts("GEM clock MMIO/lease/committed-error/drift cleanup matrix: PASS");
}
static int qlease(void *cookie)
{ return giulia_gem_clock_check(((struct fake *)cookie)->clock); }
static int qread(void *cookie,unsigned b,uint32_t off,uint32_t *v)
{
    struct fake *f=cookie; assert(b<3);
    assert(off==(b==0 ? 0x52000u : b==1 ? 0xb008u:0xb7008u));
    assert(f->clock->state==GIULIA_GEM_ACTIVE && (f->cmd&(FORCE|OFF))==FORCE);
    f->q_reads++; *v=f->q[b]; return 0;
}
static int qwrite(void *cookie,unsigned b,uint32_t off,uint32_t v)
{
    struct fake *f=cookie; uint32_t unused;
    assert(!qread(cookie,b,off,&unused)); f->q_writes++;
    f->q[b]=v; /* Can commit before reporting an error. */
    return f->q_writes==f->q_fail_write ? -84:0;
}
static void composition(void)
{
    struct fake f; struct giulia_gem_clock p; struct giulia_pcie_qos q;
    for (unsigned hid=0;hid<2;hid++) for (unsigned fail=0;fail<3;fail++) {
        struct giulia_gem_clock_io io=init(&f,&p); memset(&q,0,sizeof(q));
        f.cfg=f.initial_cfg=0x100000u|hid;
        struct giulia_pcie_qos_io qi={.cookie=&f,.read32=qread,.write32=qwrite,
            .barrier=barrier,.validate_lease=qlease,.phys={0x100000,0x16c0000,0x24100000},
            .size={0x1f4200,0x12200,0xc5080}};
        /* NoC cannot access registers before the actual clock is active. */
        assert(giulia_pcie_qos_start(&q,&qi,GIULIA_QOS_REQUIRED)==GIULIA_QOS_LEASE);
        assert(!f.q_reads && !f.q_writes);
        assert(!giulia_gem_clock_start(&p,&io));
        if (fail==1) f.q_fail_write=4;
        int rc=giulia_pcie_qos_start(&q,&qi,GIULIA_QOS_REQUIRED);
        assert(fail==1 ? rc<0 : rc==0);
        if (fail==2) {
            f.cmd|=OFF; f.stuck_off=1;
            unsigned n=f.q_reads;
            assert(giulia_pcie_qos_check(&q)<0); assert(f.q_reads==n);
            assert(giulia_pcie_qos_stop(&q)<0); assert(f.q_reads==n);
            assert(p.state==GIULIA_GEM_FAULT && q.state==GIULIA_QOS_FAULT);
            /* Model the external owner's repair. The next lease must check
             * actual config/force/ON again before ANY resumed NoC access. */
            f.stuck_off=0; f.cmd&=~OFF;
        }
        assert(!giulia_pcie_qos_stop(&q));
        assert(!f.q[0] && !f.q[1] && !f.q[2]);
        /* Parent/voltage survives until QoS restoration and clock release. */
        f.barriers=f.writes;
        assert(!giulia_gem_clock_stop(&p)); stopped(&f,&p,0);
    }
    puts("GEM clock + QoS actual-core ordering/dependency-loss: PASS");
}
int main(void)
{ admission(); success(); inherited_xo(); fault_matrix(); composition(); return 0; }
