/* SPDX-License-Identifier: BSD-2-Clause */
/* Cloud-only MMIO fixture linked to the production QoS core, no device access. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "giulia_pcie_qos.h"
#define BIT(n) (1u<<(n))
#define CLOCKS (BIT(12)|BIT(20))
#define QMASK (BIT(24)|0x78u)
struct event { unsigned block; uint32_t value; };
struct fake {
    uint32_t regs[3], original[3];
    unsigned reads,writes,leases,barriers,noc_reads;
    unsigned fail_read,fail_write,fail_lease,ones_read,drop_write;
    unsigned commit_error,persistent_lease;
    struct event events[80];
};
static const uint32_t masks[3]={CLOCKS,QMASK,QMASK};
static void access(struct fake *f, unsigned block, uint32_t off)
{
    assert(block<3);
    assert(off==(block==0 ? 0x52000u : block==1 ? 0xb008u : 0xb7008u));
    if (block) assert((f->regs[0]&CLOCKS)==CLOCKS);
}
static int read32(void *cookie,unsigned b,uint32_t off,uint32_t *v)
{
    struct fake *f=cookie; access(f,b,off); f->reads++;
    if (b) f->noc_reads++;
    assert(f->reads<500);
    if (f->reads==f->fail_read) return -91;
    *v=f->reads==f->ones_read ? UINT32_MAX : f->regs[b];
    return 0;
}
static int write32(void *cookie,unsigned b,uint32_t off,uint32_t v)
{
    struct fake *f=cookie; access(f,b,off);
    assert(f->writes<80 && ((v^f->regs[b])&~masks[b])==0);
    f->events[f->writes++]=(struct event){b,v};
    if (f->writes==f->drop_write) return 0;
    if (f->writes==f->fail_write) {
        if (f->commit_error) f->regs[b]=v;
        return -92;
    }
    f->regs[b]=v; return 0;
}
static void barrier(void *cookie) { ((struct fake *)cookie)->barriers++; }
static int validate(void *cookie)
{
    struct fake *f=cookie; f->leases++;
    return f->fail_lease && (f->leases==f->fail_lease ||
        (f->persistent_lease && f->leases>f->fail_lease)) ? -93 : 0;
}
static struct giulia_pcie_qos_io init(struct fake *f,struct giulia_pcie_qos *p)
{
    memset(f,0,sizeof(*f)); memset(p,0,sizeof(*p));
    f->regs[0]=BIT(5); f->regs[1]=0x41000048; f->regs[2]=0x82000010|BIT(24);
    memcpy(f->original,f->regs,sizeof(f->regs));
    return (struct giulia_pcie_qos_io){.cookie=f,.read32=read32,.write32=write32,
        .barrier=barrier,.validate_lease=validate,
        .phys={0x100000,0x16c0000,0x24100000},.size={0x1f4200,0x12200,0xc5080}};
}
static void restored(struct fake *f,struct giulia_pcie_qos *p)
{
    assert(p->state==GIULIA_QOS_STOPPED && !p->owned && !p->dirty);
    assert(!memcmp(f->regs,f->original,sizeof(f->regs)));
    assert(f->barriers==f->writes);
}
static void clear_faults(struct fake *f)
{
    f->fail_read=f->fail_write=f->fail_lease=f->ones_read=f->drop_write=0;
    f->persistent_lease=0;
}
static void test_admission(void)
{
    struct fake f; struct giulia_pcie_qos p,other={0};
    struct giulia_pcie_qos_io io=init(&f,&p),bad;
    for (unsigned bit=0;bit<7;bit++)
        assert(giulia_pcie_qos_start(&p,&io,GIULIA_QOS_REQUIRED&~BIT(bit))==GIULIA_QOS_PREREQUISITE);
    for (unsigned block=0;block<3;block++) {
        bad=io; bad.phys[block]^=0x1000;
        assert(giulia_pcie_qos_start(&p,&bad,GIULIA_QOS_REQUIRED)==GIULIA_QOS_LAYOUT);
        bad=io; bad.size[block]-=4;
        assert(giulia_pcie_qos_start(&p,&bad,GIULIA_QOS_REQUIRED)==GIULIA_QOS_LAYOUT);
    }
    bad=io; bad.validate_lease=0;
    assert(giulia_pcie_qos_start(&p,&bad,GIULIA_QOS_REQUIRED)==GIULIA_QOS_ARGUMENT);
    assert(!f.reads && !f.writes);
    f.fail_lease=1;
    assert(giulia_pcie_qos_start(&p,&io,GIULIA_QOS_REQUIRED)==GIULIA_QOS_LEASE);
    assert(!f.reads && !f.writes && !p.owned);
    clear_faults(&f);
    assert(!giulia_pcie_qos_start(&p,&io,GIULIA_QOS_REQUIRED));
    unsigned reads=f.reads,writes=f.writes;
    assert(giulia_pcie_qos_start(&other,&io,GIULIA_QOS_REQUIRED)==GIULIA_QOS_STATE);
    assert(f.reads==reads && f.writes==writes);
    struct giulia_pcie_qos copied=p;
    assert(giulia_pcie_qos_stop(&copied)==GIULIA_QOS_STATE);
    assert(!giulia_pcie_qos_stop(&p)); restored(&f,&p);
}
static void test_success_and_inherited(void)
{
    struct fake f; struct giulia_pcie_qos p;
    for (unsigned inherited=0;inherited<4;inherited++) {
        struct giulia_pcie_qos_io io=init(&f,&p);
        f.regs[0]|=(inherited&1 ? BIT(12):0)|(inherited&2 ? BIT(20):0);
        f.original[0]=f.regs[0];
        assert(!giulia_pcie_qos_start(&p,&io,GIULIA_QOS_REQUIRED));
        assert(p.state==GIULIA_QOS_ACTIVE && p.owned);
        assert((f.regs[1]&QMASK)==0x30 && (f.regs[2]&QMASK)==0x28);
        unsigned clock_writes=(inherited&1 ? 0:1)+(inherited&2 ? 0:1);
        assert(f.writes==clock_writes+6);
        for (unsigned j=0;j<clock_writes;j++) assert(f.events[j].block==0);
        for (unsigned j=0;j<3;j++) {
            assert(f.events[clock_writes+j].block==2);
            assert(f.events[clock_writes+3+j].block==1);
        }
        assert((f.events[clock_writes].value&QMASK)==0x10);
        assert((f.events[clock_writes+1].value&QMASK)==0x20);
        assert((f.events[clock_writes+2].value&QMASK)==0x28);
        unsigned writes=f.writes;
        assert(!giulia_pcie_qos_check(&p) && f.writes==writes);
        /* Serialized unrelated fields may evolve; rollback must preserve. */
        for (unsigned j=0;j<3;j++) f.original[j]=f.regs[j]^=BIT(7);
        f.original[0]=(f.original[0]&~CLOCKS)|(p.saved[0]&CLOCKS);
        f.original[1]=(f.original[1]&~QMASK)|(p.saved[2]&QMASK);
        f.original[2]=(f.original[2]&~QMASK)|(p.saved[1]&QMASK);
        assert(!giulia_pcie_qos_stop(&p)); restored(&f,&p);
        assert(f.events[writes].block==1 && f.events[writes+1].block==2);
        if (inherited!=3) assert(f.events[writes+2].block==0);
        assert(!giulia_pcie_qos_stop(&p));
    }
    struct giulia_pcie_qos_io io=init(&f,&p);
    f.regs[0]|=CLOCKS; f.regs[1]=(f.regs[1]&~QMASK)|0x30;
    f.regs[2]=(f.regs[2]&~QMASK)|0x28;
    memcpy(f.original,f.regs,sizeof(f.regs));
    assert(!giulia_pcie_qos_start(&p,&io,GIULIA_QOS_REQUIRED));
    assert(!f.writes && !p.dirty && p.owned);
    assert(!giulia_pcie_qos_stop(&p)); restored(&f,&p);
}
static void test_start_faults(void)
{
    struct fake f; struct giulia_pcie_qos p;
    struct giulia_pcie_qos_io io=init(&f,&p);
    assert(!giulia_pcie_qos_start(&p,&io,GIULIA_QOS_REQUIRED));
    unsigned reads=f.reads,writes=f.writes,leases=f.leases;
    assert(!giulia_pcie_qos_stop(&p));
    for (unsigned mode=0;mode<5;mode++) {
        unsigned n=mode<2 ? reads : writes;
        for (unsigned j=1;j<=n;j++) {
            io=init(&f,&p);
            if (mode==0) f.fail_read=j;
            else if (mode==1) f.ones_read=j;
            else if (mode==2) f.drop_write=j;
            else { f.fail_write=j; f.commit_error=mode==4; }
            assert(giulia_pcie_qos_start(&p,&io,GIULIA_QOS_REQUIRED)<0);
            assert(p.error<0 && p.observed_valid<=1);
            clear_faults(&f);
            assert(!giulia_pcie_qos_stop(&p)); restored(&f,&p);
        }
    }
    for (unsigned persistent=0;persistent<2;persistent++)
        for (unsigned j=1;j<=leases;j++) {
            io=init(&f,&p); f.fail_lease=j; f.persistent_lease=persistent;
            assert(giulia_pcie_qos_start(&p,&io,GIULIA_QOS_REQUIRED)==GIULIA_QOS_LEASE);
            if (persistent && j>1) assert(p.owned && p.state==GIULIA_QOS_FAULT);
            clear_faults(&f);
            assert(!giulia_pcie_qos_stop(&p));
            if (j==1) { assert(p.state==GIULIA_QOS_IDLE && !f.writes); }
            else restored(&f,&p);
        }
    puts("Ace5 PCIe QoS: all start read/write/readback/lease boundaries recover without collateral writes");
}
static void test_stop_faults(void)
{
    struct fake f; struct giulia_pcie_qos p;
    for (unsigned mode=0;mode<3;mode++) for (unsigned step=1;step<=3;step++) {
        struct giulia_pcie_qos_io io=init(&f,&p);
        assert(!giulia_pcie_qos_start(&p,&io,GIULIA_QOS_REQUIRED));
        if (mode==0) f.drop_write=f.writes+step;
        else { f.fail_write=f.writes+step; f.commit_error=mode==2; }
        assert(giulia_pcie_qos_stop(&p)<0);
        assert(p.state==GIULIA_QOS_FAULT && p.owned && p.dirty && p.cleanup_error<0);
        if (step<3) assert((f.regs[0]&CLOCKS)==CLOCKS);
        if (step>1) assert(!(p.dirty&BIT(2)));
        if (step>2) assert(!(p.dirty&BIT(1)));
        unsigned writes=f.writes;
        clear_faults(&f); assert(!giulia_pcie_qos_stop(&p)); restored(&f,&p);
        if (step>1) for (unsigned k=writes;k<f.writes;k++) assert(f.events[k].block!=1);
    }
    struct giulia_pcie_qos_io io=init(&f,&p);
    assert(!giulia_pcie_qos_start(&p,&io,GIULIA_QOS_REQUIRED));
    unsigned base=f.reads;
    assert(!giulia_pcie_qos_stop(&p)); unsigned stop_reads=f.reads-base;
    for (unsigned j=1;j<=stop_reads;j++) {
        io=init(&f,&p); assert(!giulia_pcie_qos_start(&p,&io,GIULIA_QOS_REQUIRED));
        f.fail_read=f.reads+j;
        assert(giulia_pcie_qos_stop(&p)==GIULIA_QOS_IO);
        assert(p.owned && p.dirty && p.state==GIULIA_QOS_FAULT);
        clear_faults(&f); assert(!giulia_pcie_qos_stop(&p)); restored(&f,&p);
    }
    puts("Ace5 PCIe QoS: partial restore keeps clocks and retries only remaining register fields");
}
static void test_drift_and_access_loss(void)
{
    struct fake f; struct giulia_pcie_qos p;
    for (unsigned b=0;b<3;b++) {
        struct giulia_pcie_qos_io io=init(&f,&p);
        assert(!giulia_pcie_qos_start(&p,&io,GIULIA_QOS_REQUIRED));
        f.regs[b]^= b ? BIT(4) : BIT(20);
        unsigned writes=f.writes,noc=f.noc_reads;
        assert(giulia_pcie_qos_check(&p)==GIULIA_QOS_READBACK);
        assert(p.state==GIULIA_QOS_FAULT && f.writes==writes);
        if (!b) {
            assert(f.noc_reads==noc);
            assert(giulia_pcie_qos_stop(&p)==GIULIA_QOS_READBACK);
            assert(f.noc_reads==noc && f.writes==writes && p.dirty);
            f.regs[0]|=CLOCKS; /* Actual owner repairs access before retry. */
        }
        assert(!giulia_pcie_qos_stop(&p)); restored(&f,&p);
    }
    struct giulia_pcie_qos_io io=init(&f,&p);
    assert(!giulia_pcie_qos_start(&p,&io,GIULIA_QOS_REQUIRED));
    f.fail_lease=f.leases+1; f.persistent_lease=1;
    unsigned reads=f.reads,writes=f.writes;
    assert(giulia_pcie_qos_check(&p)==GIULIA_QOS_LEASE);
    assert(giulia_pcie_qos_stop(&p)==GIULIA_QOS_LEASE);
    assert(f.reads==reads && f.writes==writes && p.owned);
    clear_faults(&f); assert(!giulia_pcie_qos_stop(&p)); restored(&f,&p);
    puts("Ace5 PCIe QoS: access loss blocks NoC transactions and retained ownership prevents a second writer");
}
int main(void)
{
    test_admission(); test_success_and_inherited(); test_start_faults();
    test_stop_faults(); test_drift_and_access_loss();
    puts("Ace5 PCIe QoS: actual ANOC/GEM fields, branch votes, readback and baseline restoration passed");
    return 0;
}
