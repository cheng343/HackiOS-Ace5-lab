/* SPDX-License-Identifier: BSD-2-Clause */
/* Real ARC/RPMh/GEM-power/GEM-clock/QoS cores, synthetic peripherals only. */
#define GIULIA_RPMH_GEM_TEST
#define main original_rpmh_test_main
#include "test-rpmh.c"
#undef main
#include "giulia_gem_power.h"
#include "giulia_pcie_qos.h"
struct board {
    struct mock rsc;
    struct giulia_gem_power *power;
    uint32_t cmd,cfg,qos[3];
    unsigned gcc_reads,gcc_writes,qos_reads,qos_writes,off_stuck,fail_lease,leases;
};
static void arc_database(uint8_t db[1024])
{
    /* Deliberately shifted addresses and selector2 != corner64. */
    memset(db,0,1024); le32(db,9); le32(db+4,0x0c0330db);
    le16(db+8,3); le16(db+12,128); le16(db+14,2);
    const uint16_t levels[2][6]={{0,32,80,128,0,0},{0,16,64,128,256,0}};
    for (unsigned i=0;i<2;i++) {
        uint8_t *e=db+144+i*24,*a=db+272+i*32;
        memcpy(e,i ? "cx.lvl":"xo.lvl",6); le32(e+16,0x31000+i*16);
        le16(e+20,32); le16(e+22,i*32);
        for (unsigned j=0;j<6;j++) le16(a+2*j,levels[i][j]);
    }
}
static int platform(void *cookie)
{
    struct board *b=cookie; return ++b->leases==b->fail_lease ? -1:0;
}
static int gr(void *cookie,uint32_t off,uint32_t *v)
{
    struct board *b=cookie; b->gcc_reads++;
    assert(off==0x63004 || off==0x63008);
    assert(b->rsc.arc_vote[0]>=3 && b->rsc.arc_vote[1]>=2);
    *v=off==0x63004 ? b->cmd:b->cfg; return 0;
}
static int gw(void *cookie,uint32_t off,uint32_t v)
{
    struct board *b=cookie; assert(off==0x63004 && ((v^b->cmd)&~BIT(1))==0);
    assert(b->rsc.arc_vote[0]>=3 && b->rsc.arc_vote[1]>=2);
    b->gcc_writes++; b->cmd=v;
    if ((v&BIT(1)) && !b->off_stuck) b->cmd&=~BIT(31);
    return 0;
}
static void gb(void *cookie) { (void)cookie; }
static void gd(void *cookie,unsigned us) { (void)cookie; assert(us==1); }
static int qlease(void *cookie) { return giulia_gem_power_check(((struct board *)cookie)->power); }
static int qr(void *cookie,unsigned n,uint32_t off,uint32_t *v)
{
    struct board *b=cookie;
    assert(n<3 && off==(n==0 ? 0x52000u:n==1 ? 0xb008u:0xb7008u));
    assert((b->cmd&(BIT(31)|BIT(1)))==BIT(1)); b->qos_reads++; *v=b->qos[n]; return 0;
}
static int qw(void *cookie,unsigned n,uint32_t off,uint32_t v)
{
    struct board *b=cookie; uint32_t unused; qr(cookie,n,off,&unused);
    b->qos_writes++; b->qos[n]=v; return 0;
}
static struct giulia_gem_clock_io prepare(struct board *b,struct giulia_rpmh *t,
    struct giulia_gem_power *p)
{
    memset(b,0,sizeof(*b)); memset(p,0,sizeof(*p)); setup(&b->rsc,t);
    b->cmd=BIT(31); b->cfg=0x100001; b->power=p;
    return (struct giulia_gem_clock_io){.cookie=b,.read32=gr,.write32=gw,.barrier=gb,
        .delay_us=gd,.validate_lease=platform,.gcc_phys=0x100000,.gcc_size=0x1f4200};
}
static void check_stop(struct board *b,struct giulia_rpmh *t,struct giulia_gem_power *p,
    const struct giulia_gem_power_baseline *baseline,uint32_t expected_cfg)
{
    assert(!giulia_gem_power_stop(p));
    assert(p->state==GIULIA_GEM_POWER_STOPPED && !p->owned && !p->dirty && !p->pending);
    assert(!(b->cmd&BIT(1)) && b->cfg==expected_cfg);
    for (unsigned i=0;i<2;i++) assert(b->rsc.arc_vote[i]==baseline->selector[i]);
    if (t->state==GIULIA_RPMH_IDLE) {
        /* Borrowed sufficient requests never acquire or touch the TCS. */
        assert(!b->rsc.reads && !b->rsc.writes && !b->rsc.triggers);
        assert(!t->dirty && !giulia_rpmh_bus_owned(t));
        assert(!memcmp(b->rsc.regs,b->rsc.original,sizeof(b->rsc.regs)));
    } else restored(&b->rsc,t);
}
static void parser(void)
{
    uint8_t db[1024],bad[1024]; struct giulia_arc_resource r; uint32_t s;
    arc_database(db); assert(!giulia_arc_resolve(db,sizeof(db),9,"cx.lvl",&r));
    assert(r.address==0x31010 && r.count==5 && r.level[2]==64);
    assert(!giulia_arc_select(&r,64,&s) && s==2);
    assert(!giulia_arc_select(&r,65,&s) && s==3);
    assert(giulia_arc_select(&r,257,&s)==GIULIA_RPMH_NOT_FOUND && s==UINT32_MAX);
    for (unsigned n=0;n<144;n++) assert(giulia_arc_resolve(db,n,9,"cx.lvl",&r)<0 && !r.address);
    for (unsigned mode=0;mode<13;mode++) {
        memcpy(bad,db,sizeof(db));
        if (mode==0) le32(bad,10);
        if (mode==1) le32(bad+4,0);
        if (mode==2) le16(bad+8,4);
        if (mode==3) le32(bad+144+24+16,0x41010);
        if (mode==4) le16(bad+144+24+20,31);
        if (mode==5) le16(bad+144+24+20,34);
        if (mode==6) le16(bad+144+24+22,1023);
        if (mode==7) le16(bad+304+6,63); /* descending */
        if (mode==8) le16(bad+304+6,64); /* duplicate */
        if (mode==9) le16(bad+304+12,320); /* nonzero after padding */
        if (mode==10) memcpy(bad+144,"cx.lvl",6); /* duplicate resource */
        if (mode==11) { memcpy(bad+24,bad+8,16); memset(bad+8,0,16); }
        if (mode==12) le32(bad+144+24+16,0x31011);
        assert(giulia_arc_resolve(bad,sizeof(bad),9,"cx.lvl",&r)<0 && !r.address && !r.count);
    }
    /* Changing the actual table changes the payload; no hardcoded selector. */
    le16(db+304+4,32); le16(db+304+6,64);
    assert(!giulia_arc_resolve(db,sizeof(db),9,"cx.lvl",&r));
    assert(!giulia_arc_select(&r,64,&s) && s==3);
    puts("Ace5 ARC: bounded CMD DB levels, selector conversion and malformed tables passed");
}
static void flow(void)
{
    struct board b; struct giulia_rpmh t; struct giulia_gem_power p;
    uint8_t db[1024]; arc_database(db);
    for (unsigned hid=0;hid<2;hid++) for (unsigned prior=0;prior<3;prior++) {
        struct giulia_gem_clock_io io=prepare(&b,&t,&p);
        b.cfg=0x100000u|hid;
        struct giulia_gem_power_lease l={&b,platform};
        struct giulia_gem_power_baseline base={{prior ? 3u:0u,prior==2 ? 4u:0u},3};
        memcpy(b.rsc.arc_vote,base.selector,sizeof(base.selector));
        assert(!giulia_gem_power_start(&p,&t,&l,&io,db,sizeof(db),9,&base));
        assert(p.state==GIULIA_GEM_POWER_ACTIVE && p.held==3);
        assert(b.rsc.arc_vote[0]==3 && b.rsc.arc_vote[1]==(prior==2 ? 4u:2u));
        struct giulia_gem_power other={0};
        assert(giulia_gem_power_start(&other,&t,&l,&io,db,sizeof(db),9,&base)==GIULIA_RPMH_BUSY);
        struct giulia_pcie_qos q={0};
        struct giulia_pcie_qos_io qi={.cookie=&b,.read32=qr,.write32=qw,.barrier=gb,.validate_lease=qlease,
            .phys={0x100000,0x16c0000,0x24100000},.size={0x1f4200,0x12200,0xc5080}};
        assert(!giulia_pcie_qos_start(&q,&qi,GIULIA_QOS_REQUIRED));
        assert(!giulia_pcie_qos_stop(&q)); assert(!b.qos[0] && !b.qos[1] && !b.qos[2]);
        check_stop(&b,&t,&p,&base,0x100000u|hid);
        if (!prior) {
            assert(b.rsc.triggers==4);
            const uint32_t addresses[]={0x31000,0x31010,0x31010,0x31000};
            const uint32_t payloads[]={3,2,0,0};
            for (unsigned i=0;i<4;i++) {
                assert(b.rsc.count[i]==1 && b.rsc.command[i][0]==addresses[i]);
                assert(b.rsc.arc_data[i]==payloads[i]);
            }
        } else assert(b.rsc.triggers==(prior==2 ? 0u:2u));
    }
    puts("Ace5 GEM power: actual RPMh -> clock -> QoS, shared higher votes and reverse restore passed");
}
static void failures(void)
{
    struct board b; struct giulia_rpmh t; struct giulia_gem_power p;
    uint8_t db[1024]; arc_database(db);
    struct giulia_gem_power_baseline base={{0,0},3};
    for (unsigned stage=1;stage<=4;stage++) {
        struct giulia_gem_clock_io io=prepare(&b,&t,&p);
        struct giulia_gem_power_lease l={&b,platform};
        b.rsc.hold_at=stage;
        int rc=giulia_gem_power_start(&p,&t,&l,&io,db,sizeof(db),9,&base);
        if (stage<=2) assert(rc==GIULIA_RPMH_TIMEOUT && !b.gcc_reads && !b.gcc_writes);
        else { assert(!rc); assert(giulia_gem_power_stop(&p)==GIULIA_RPMH_TIMEOUT); }
        assert(p.pending && p.owned && p.state==GIULIA_GEM_POWER_FAULT);
        unsigned triggers=b.rsc.triggers;
        assert(giulia_gem_power_stop(&p)==GIULIA_RPMH_TIMEOUT && b.rsc.triggers==triggers);
        b.rsc.hold_at=0; check_stop(&b,&t,&p,&base,0x100001);
        assert(b.rsc.triggers==(stage==1 ? 2u:4u));
    }
    for (unsigned mode=0;mode<5;mode++) {
        struct giulia_gem_clock_io io=prepare(&b,&t,&p);
        struct giulia_gem_power_lease l={&b,platform};
        if (mode==0) base.known=0;
        if (mode==1) b.rsc.drop=MSG;
        if (mode==2) b.rsc.cleanup_block=1;
        if (mode==3) b.off_stuck=1;
        if (mode==4) b.fail_lease=3;
        assert(giulia_gem_power_start(&p,&t,&l,&io,db,sizeof(db),9,&base)<0);
        if (mode==0 || mode==4) assert(!b.rsc.writes && !b.gcc_writes);
        b.rsc.drop=b.rsc.cleanup_block=b.fail_lease=0; base.known=3;
        if (p.state!=GIULIA_GEM_POWER_IDLE) check_stop(&b,&t,&p,&base,0x100001);
    }
    puts("Ace5 GEM power: four transaction timeouts never resend; failed clock/cleanup retains dependencies passed");
}
int main(void) { parser(); flow(); failures(); return 0; }
