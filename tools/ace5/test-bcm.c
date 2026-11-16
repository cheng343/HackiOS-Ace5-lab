/* SPDX-License-Identifier: BSD-2-Clause */
/* Cloud-only: synthetic CMD DB -> production BCM -> production CRM -> MMIO. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "giulia_bcm.h"
static void le16(uint8_t *p,unsigned v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void le32(uint8_t *p,uint32_t v) { le16(p,v); le16(p+2,v>>16); }
static void database(uint8_t db[1024])
{
    const char *names[]={"MC0","SH0","SH1","SN4","ACV"};
    memset(db,0,1024); le32(db,1); le32(db+4,0x0c0330db);
    le16(db+8,5); le16(db+12,256); le16(db+14,5);
    for (unsigned i=0;i<5;i++) {
        uint8_t *e=db+144+24*i,*a=db+400+8*i;
        memcpy(e,names[i],strlen(names[i])); le32(e+16,0x50000+4*i);
        le16(e+20,8); le16(e+22,i*8); le32(a,1000); le16(a+4,16); a[6]=(uint8_t)i;
    }
}
static void decoder(void)
{
    uint8_t db[1024]; struct giulia_bcm_resource r;
    database(db);
    assert(giulia_bcm_resolve(db,sizeof(db),1,"MC0",&r)==0);
    assert(r.address==0x50000 && r.unit==1000 && r.width==16 && !r.vcd);
    assert(giulia_bcm_resolve(db,sizeof(db),1,"NOPE",&r)==GIULIA_BCM_NOT_FOUND && !r.address);
    for (unsigned n=0;n<440;n++)
        assert(giulia_bcm_resolve(db,n,1,"MC0",&r)==GIULIA_BCM_DATABASE);
    assert(giulia_bcm_resolve(db,sizeof(db),2,"MC0",&r)==GIULIA_BCM_DATABASE);
    le16(db+144+20,7);
    assert(giulia_bcm_resolve(db,sizeof(db),1,"MC0",&r)==GIULIA_BCM_DATABASE);
    database(db); le16(db+144+22,900);
    assert(giulia_bcm_resolve(db,sizeof(db),1,"MC0",&r)==GIULIA_BCM_DATABASE);
    database(db); le32(db+160,0x40000);
    assert(giulia_bcm_resolve(db,sizeof(db),1,"MC0",&r)==GIULIA_BCM_DATABASE);
    database(db); memcpy(db+168,db+144,24);
    assert(giulia_bcm_resolve(db,sizeof(db),1,"MC0",&r)==GIULIA_BCM_DATABASE);
    database(db); memcpy(db+24,db+8,16); memset(db+8,0,16);
    assert(giulia_bcm_resolve(db,sizeof(db),1,"MC0",&r)==GIULIA_BCM_DATABASE);
    database(db); memset(db+400,0,6);
    assert(giulia_bcm_resolve(db,sizeof(db),1,"MC0",&r)==0 && r.unit==1 && r.width==1);
}
static void encoding(void)
{
    struct giulia_bcm_resource r={0x50000,3,5,0}; struct giulia_bcm_vote v;
    assert(giulia_bcm_encode(&r,1,2,3,1,0,&v)==0);
    assert(v.x==333 && v.y==1000 && v.command==(0x60000000u|(333u<<14)|1000) && !v.saturated);
    r.unit=1000000; r.width=1;
    assert(giulia_bcm_encode(&r,1,1,16,4,0,&v)==0 && v.x==1 && v.y==1);
    assert(giulia_bcm_encode(&r,0,0,16,4,0,&v)==0 && v.command==0x60000000u);
    assert(giulia_bcm_encode(&r,UINT64_MAX,UINT64_MAX,16,4,1,&v)==0 && v.x==0 && v.y==1);
    assert(giulia_bcm_encode(&r,0,0,16,4,1,&v)==0 && v.command==0x60000000u);
    r.unit=1; r.width=16;
    assert(giulia_bcm_encode(&r,1000000,1000000,16,1,0,&v)==0);
    assert(v.saturated && v.x==0x3fff && v.y==0x3fff && v.command==0x6fffffffu);
    assert(giulia_bcm_encode(&r,UINT64_MAX,0,16,1,0,&v)==GIULIA_BCM_OVERFLOW);
    r.width=1;
    assert(giulia_bcm_encode(&r,UINT64_MAX,0,1,1,0,&v)==GIULIA_BCM_OVERFLOW);
    assert(giulia_bcm_encode(&r,1,1,UINT32_MAX,2,0,&v)==GIULIA_BCM_ARGUMENT);
}
static void plans(void)
{
    uint8_t db[1024]; struct giulia_bcm_pcie0_plan p;
    static const unsigned x[]={500,125,0,500,0},y[]={3200,800,1,800,1},nodes[]={0,1,2,3,5};
    database(db);
    assert(giulia_bcm_pcie0_plan(db,sizeof(db),1,GIULIA_BCM_PCIE0_FIXED_500_800,&p)==0);
    for (unsigned i=0;i<5;i++) {
        assert(p.vote[i].x==x[i] && p.vote[i].y==y[i] && !p.vote[i].saturated);
        for (unsigned st=0;st<2;st++) {
            struct giulia_crm_patch *q=&p.patch[2+2*i+st];
            assert(q->kind==1 && q->state==st && q->resource==nodes[i]);
            assert(q->mask==GIULIA_CRM_BW_FIELDS && q->value==(p.vote[i].command&GIULIA_CRM_BW_FIELDS));
            assert(!(q->value&(3u<<30)));
        }
    }
    for (unsigned i=0;i<2;i++) assert(p.patch[i].kind==0 && p.patch[i].state==i && p.patch[i].value==2);
    assert(giulia_bcm_pcie0_plan(db,sizeof(db),1,GIULIA_BCM_PCIE0_GEN1_X1_250000_0,&p)==0);
    assert(p.vote[0].saturated && p.vote[1].saturated && p.vote[3].saturated);
    assert(!p.vote[0].y && p.vote[2].y==1 && p.vote[4].y==1);
    le32(db+184,0x50000); /* Distinct BCM names cannot alias one address. */
    assert(giulia_bcm_pcie0_plan(db,sizeof(db),1,0,&p)==GIULIA_BCM_DATABASE);
    assert(!p.patch[0].value && !p.resource[0].address);
    puts("Ace5 BCM HW BW: generic TCS command converted to HW-only table fields");
}
struct mock { uint32_t reg[0x3000/4],saved[0x3000/4]; unsigned writes,switches; };
static int read32(void *cookie,uint32_t off,uint32_t *v)
{ struct mock *m=cookie; assert(!(off&3) && off<sizeof(m->reg)); *v=m->reg[off/4]; return 0; }
static int write32(void *cookie,uint32_t off,uint32_t v)
{
    struct mock *m=cookie; assert(!(off&3) && off<sizeof(m->reg)); m->writes++;
    static const uint32_t bw[]={0xc0,0x140,0x680,0x700,0x780};
    for (unsigned ch=0;ch<2;ch++) for (unsigned st=0;st<5;st++) for (unsigned i=0;i<16;i++)
        if (off==0x200+bw[st]+4*(ch*16+i)) v&=~(1u<<30);
    m->reg[off/4]=v;
    if (off==0x1020) { m->reg[0x1000/4]=v; m->switches++; }
    return 0;
}
static void barrier(void *cookie) { (void)cookie; }
static void delay(void *cookie,unsigned us) { (void)cookie; (void)us; assert(!"unexpected wait"); }
static int lease(void *cookie) { (void)cookie; return 0; }
static void transport(void)
{
    uint8_t db[1024]; struct mock m={0}; struct giulia_crm crm={0}; struct giulia_bcm_pcie0_plan p;
    struct giulia_crm_io io={&m,read32,write32,barrier,delay,lease};
    database(db); m.reg[0]=0x10000; m.reg[1]=0x1228225; m.reg[2]=0x40108128; m.reg[3]=1;
    m.reg[0x1000/4]=m.reg[0x1020/4]=1;
    for (unsigned i=0x200/4;i<0xA00/4;i++) m.reg[i]=0x90000005u;
    memcpy(m.saved,m.reg,sizeof(m.reg));
    assert(giulia_bcm_pcie0_start(&crm,&io,db,sizeof(db),1,0,&p)==0);
    assert(m.switches==1 && crm.state==GIULIA_CRM_ACTIVE);
    assert(m.reg[(0x200+8)/4]==((0x90000005u&~7u)|2u)); /* PERF ch1, state0 */
    for (unsigned i=0;i<5;i++) {
        unsigned node=i==4?5:i;
        assert(m.reg[(0x200+0xc0+16*4+node*4)/4]==
            ((0x90000005u&~GIULIA_CRM_BW_FIELDS)|(p.vote[i].command&GIULIA_CRM_BW_FIELDS)));
    }
    assert(m.reg[(0x200+0xc0+16*4+4*4)/4]==0x90000005u); /* IP0 preserved. */
    assert(m.reg[(0x200+0x680+16*4)/4]==0x90000005u); /* state2 preserved. */
    assert(giulia_crm_stop(&crm)==0 && m.switches==2);
    assert(!memcmp(m.reg,m.saved,sizeof(m.reg)));
    unsigned writes=m.writes; db[4]^=1;
    assert(giulia_bcm_pcie0_start(&crm,&io,db,sizeof(db),1,0,&p)==GIULIA_BCM_DATABASE && m.writes==writes);
}
static void empty_transport_last(void)
{
    uint8_t db[1024]; static struct mock m; static struct giulia_crm crm;
    struct giulia_bcm_pcie0_plan p;
    struct giulia_crm_io io={&m,read32,write32,barrier,delay,lease};
    /* Unit/width pairs checked against the observed Ace5 CMD DB. Entry
     * addresses and the surrounding database remain synthetic fixture data. */
    const unsigned units[]={4700000,4800000,4800000,530000,1},widths[]={16,64,64,16,1};
    database(db);
    for (unsigned i=0;i<5;i++) { le32(db+400+8*i,units[i]); le16(db+404+8*i,widths[i]); }
    m.reg[0]=0x10000; m.reg[1]=0x1228225; m.reg[2]=0x40108128; m.reg[3]=1;
    assert(giulia_bcm_pcie0_start(&crm,&io,db,sizeof(db),1,0,&p)==GIULIA_CRM_BUSY && !m.writes);
    assert(giulia_bcm_pcie0_start_empty_keep(&crm,&io,db,sizeof(db),1,0,&p)==0);
    for (unsigned i=0;i<5;i++) {
        assert(p.vote[i].y==1 && p.vote[i].x==(i==2 || i==4 ? 0u : 1u));
        assert(!p.vote[i].saturated);
    }
    assert(m.switches==1 && crm.original==2 && crm.target==0);
    unsigned writes=m.writes;
    assert(giulia_crm_stop(&crm)==GIULIA_CRM_RETAINED && crm.owned && crm.state==GIULIA_CRM_HELD);
    assert(m.writes==writes && m.switches==1 && m.reg[0x1020/4]==1);
    puts("Ace5 BCM empty: observed aux scaling to real CRM first-channel initialization passed");
}
int main(void)
{
    decoder(); encoding(); plans(); transport();
    empty_transport_last();
    puts("Ace5 BCM: real CMD DB aux, vendor bandwidth conversion and integrated CRM transfer passed");
    return 0;
}
