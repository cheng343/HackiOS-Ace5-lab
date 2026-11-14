/* SPDX-License-Identifier: BSD-2-Clause */
/* Cloud-only: production CRM transport, synthetic two-channel MMIO. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "giulia_crm.h"
#define BUSY 0x1000u
#define UPDATE 0x1020u
struct mock {
    uint32_t reg[0x3000/4], original[0x3000/4];
    unsigned reads,writes,table_writes,switches,delays,lease_calls;
    unsigned stall,drop_table,post_error,deny_lease,fail_read;
    unsigned perf,bw;
};
static uint32_t address(unsigned kind,unsigned ch,unsigned state,unsigned i,unsigned count)
{
    static const uint32_t perf[] = {0,0x40,0x80,0x600,0x640};
    static const uint32_t bw[] = {0xc0,0x140,0x680,0x700,0x780};
    return 0x200+(kind ? bw[state] : perf[state])+(ch*count+i)*4;
}
static int table_channel(struct mock *m,uint32_t off)
{
    for (unsigned k=0;k<2;k++) for (unsigned ch=0;ch<2;ch++)
        for (unsigned s=0;s<5;s++) for (unsigned i=0;i<(k?m->bw:m->perf);i++)
            if (off == address(k,ch,s,i,k?m->bw:m->perf)) return (int)ch;
    return -1;
}
static int read32(void *cookie,uint32_t off,uint32_t *v)
{
    struct mock *m=cookie;
    assert(!(off&3) && off<sizeof(m->reg));
    assert(off<16 || off==BUSY || off==UPDATE || table_channel(m,off)>=0);
    m->reads++;
    if (m->reads==m->fail_read) return -99;
    *v=m->reg[off/4]; return 0;
}
static int write32(void *cookie,uint32_t off,uint32_t v)
{
    struct mock *m=cookie; m->writes++;
    if (off==UPDATE) {
        assert(v==1 || v==2); m->switches++;
        m->reg[UPDATE/4]=v;
        if (!m->stall) m->reg[BUSY/4]=v;
        return m->post_error ? -91 : 0;
    }
    int ch=table_channel(m,off);
    assert(ch>=0 && m->reg[BUSY/4]==m->reg[UPDATE/4]);
    assert(m->reg[BUSY/4] != (1u<<ch)); /* Never write an active table. */
    m->table_writes++;
    if (m->table_writes != m->drop_table) m->reg[off/4]=v;
    return 0;
}
static void barrier(void *cookie) { (void)cookie; }
static void delay(void *cookie,unsigned us)
{
    struct mock *m=cookie;
    assert(us==100); assert(++m->delays<1000);
}
static int lease(void *cookie)
{
    struct mock *m=cookie; m->lease_calls++;
    return m->deny_lease ? -71 : 0;
}
static struct giulia_crm_io setup(struct mock *m,unsigned active,unsigned perf,unsigned bw)
{
    memset(m,0,sizeof(*m)); m->perf=perf; m->bw=bw;
    m->reg[0]=0x10000;
    m->reg[1]=(1u<<24)|(2u<<20)|(2u<<16)|(8u<<12)|(perf<<8)|0x25;
    m->reg[2]=bw<<26; m->reg[3]=1;
    m->reg[BUSY/4]=m->reg[UPDATE/4]=1u<<active;
    for (unsigned k=0;k<2;k++) for (unsigned ch=0;ch<2;ch++)
        for (unsigned s=0;s<5;s++) for (unsigned i=0;i<(k?bw:perf);i++)
            m->reg[address(k,ch,s,i,k?bw:perf)/4]=
                0x90000000u|(k<<29)|(ch<<24)|(s<<16)|(i<<8)|5;
    memcpy(m->original,m->reg,sizeof(m->reg));
    return (struct giulia_crm_io){m,read32,write32,barrier,delay,lease};
}
static const struct giulia_crm_patch patch[] = {
    {GIULIA_CRM_PERF,0,0,7,2}, {GIULIA_CRM_PERF,1,0,7,2},
    {GIULIA_CRM_BW,0,3,GIULIA_CRM_BW_FIELDS,0x60004002u},
    {GIULIA_CRM_BW,1,3,GIULIA_CRM_BW_FIELDS,0x60008001u}
};
static int start(struct giulia_crm *p,struct giulia_crm_io *io)
{ return giulia_crm_start(p,io,patch,4); }
static void restored(struct mock *m,struct giulia_crm *p)
{
    assert(p->state==GIULIA_CRM_STOPPED && !p->owned && !p->pending && !p->dirty);
    assert(!memcmp(m->reg,m->original,sizeof(m->reg)));
}
static void check_target(struct mock *m,unsigned active)
{
    unsigned target=1-active;
    for (unsigned k=0;k<2;k++) for (unsigned s=0;s<5;s++)
        for (unsigned i=0;i<(k?m->bw:m->perf);i++) {
            unsigned old=address(k,active,s,i,k?m->bw:m->perf);
            unsigned new=address(k,target,s,i,k?m->bw:m->perf);
            uint32_t expected=m->original[old/4];
            for (unsigned n=0;n<4;n++) if (patch[n].kind==k && patch[n].state==s && patch[n].resource==i)
                expected=(expected&~patch[n].mask)|patch[n].value;
            assert(m->reg[old/4]==m->original[old/4]);
            assert(m->reg[new/4]==expected);
        }
}
static void lifecycle(void)
{
    for (unsigned active=0;active<2;active++) for (unsigned max=0;max<2;max++) {
        struct mock m; struct giulia_crm p={0},other={0};
        struct giulia_crm_io io=setup(&m,active,max?8:2,max?16:6);
        assert(start(&p,&io)==0 && p.state==GIULIA_CRM_ACTIVE);
        assert(m.switches==1 && m.table_writes==5*(m.perf+m.bw));
        check_target(&m,active);
        assert(start(&other,&io)==GIULIA_CRM_BUSY);
        assert(giulia_crm_stop(&p)==0 && m.switches==2);
        restored(&m,&p);
        unsigned writes=m.writes;
        assert(giulia_crm_stop(&p)==0 && m.writes==writes);
        assert(start(&p,&io)==0 && giulia_crm_stop(&p)==0);
        restored(&m,&p);
    }
}
static void admission(void)
{
    struct mock m; struct giulia_crm p={0}; struct giulia_crm_patch bad[4];
    struct giulia_crm_io io=setup(&m,0,2,6);
    memcpy(bad,patch,sizeof(bad)); bad[0].mask=8;
    assert(giulia_crm_start(&p,&io,bad,4)==GIULIA_CRM_ARGUMENT && !m.reads);
    memcpy(bad,patch,sizeof(bad)); bad[1]=bad[0];
    assert(giulia_crm_start(&p,&io,bad,4)==GIULIA_CRM_ARGUMENT && !m.reads);
    memcpy(bad,patch,sizeof(bad)); bad[0].value=8;
    assert(giulia_crm_start(&p,&io,bad,4)==GIULIA_CRM_ARGUMENT && !m.reads);
    m.deny_lease=1; assert(start(&p,&io)==GIULIA_CRM_LEASE && !m.reads && !m.writes);
    m.deny_lease=0;
    const uint32_t update[]={0,3,4,UINT32_MAX};
    for (unsigned n=0;n<4;n++) {
        m.reg[UPDATE/4]=update[n];
        assert(start(&p,&io)==GIULIA_CRM_BUSY && !m.writes && !p.owned);
    }
    m.reg[UPDATE/4]=2;
    assert(start(&p,&io)==GIULIA_CRM_BUSY && !m.writes);
    m.reg[UPDATE/4]=1;
    m.reg[0]=0x10100;
    assert(start(&p,&io)==GIULIA_CRM_LAYOUT && !m.writes); m.reg[0]=0x10000;
    m.reg[3]=0;
    assert(start(&p,&io)==GIULIA_CRM_LAYOUT && !m.writes); m.reg[3]=1;
    memcpy(bad,patch,sizeof(bad)); bad[0].resource=2;
    assert(giulia_crm_start(&p,&io,bad,4)==GIULIA_CRM_ARGUMENT && !m.writes);
    unsigned at=address(0,0,1,0,2);
    m.reg[at/4]=UINT32_MAX;
    assert(start(&p,&io)==GIULIA_CRM_IO && !m.writes && !p.owned);
}
static void lost_writes(void)
{
    for (unsigned n=1;n<=40;n++) {
        struct mock m; struct giulia_crm p={0};
        struct giulia_crm_io io=setup(&m,0,2,6); m.drop_table=n;
        assert(start(&p,&io)==GIULIA_CRM_DRIFT);
        assert(p.dirty && p.owned && !m.switches);
        m.drop_table=0;
        assert(giulia_crm_stop(&p)==0); restored(&m,&p);
    }
}
static void switch_failures(void)
{
    struct mock m; struct giulia_crm p={0},other={0};
    struct giulia_crm_io io=setup(&m,0,2,6); m.stall=1;
    assert(start(&p,&io)==GIULIA_CRM_TIMEOUT && p.pending && m.switches==1);
    unsigned writes=m.writes;
    assert(giulia_crm_recover(&p)==GIULIA_CRM_TIMEOUT && m.writes==writes);
    assert(start(&other,&io)==GIULIA_CRM_BUSY);
    m.reg[BUSY/4]=m.reg[UPDATE/4]; m.stall=0;
    assert(giulia_crm_recover(&p)==0 && !p.pending && m.switches==1);
    assert(giulia_crm_stop(&p)==0 && m.switches==2); restored(&m,&p);

    io=setup(&m,1,2,6); assert(start(&p,&io)==0); m.stall=1;
    assert(giulia_crm_stop(&p)==GIULIA_CRM_TIMEOUT && p.pending && m.switches==2);
    writes=m.table_writes;
    assert(giulia_crm_stop(&p)==GIULIA_CRM_TIMEOUT && m.switches==2 && m.table_writes==writes);
    m.reg[BUSY/4]=m.reg[UPDATE/4]; m.stall=0;
    assert(giulia_crm_stop(&p)==0 && m.switches==2); restored(&m,&p);

    io=setup(&m,0,2,6); m.post_error=1;
    assert(start(&p,&io)==GIULIA_CRM_IO && p.pending && m.switches==1);
    m.post_error=0;
    assert(giulia_crm_recover(&p)==0 && m.switches==1);
    assert(giulia_crm_stop(&p)==0); restored(&m,&p);
}
static void lease_and_restore_faults(void)
{
    struct mock m; struct giulia_crm p={0}; struct giulia_crm_io io=setup(&m,0,2,6);
    assert(start(&p,&io)==0); m.deny_lease=1;
    unsigned writes=m.writes;
    assert(giulia_crm_stop(&p)==GIULIA_CRM_LEASE && p.owned && m.writes==writes);
    m.deny_lease=0;
    unsigned original_word=address(0,0,2,0,2);
    m.reg[original_word/4]^=1;
    assert(giulia_crm_stop(&p)==GIULIA_CRM_DRIFT && m.writes==writes);
    m.reg[original_word/4]^=1;
    m.drop_table=m.table_writes+3;
    assert(giulia_crm_stop(&p)==GIULIA_CRM_DRIFT && m.switches==2 && p.dirty);
    m.drop_table=0;
    assert(giulia_crm_stop(&p)==0 && m.switches==2); restored(&m,&p);

    io=setup(&m,0,2,6); m.fail_read=15;
    assert(start(&p,&io)==GIULIA_CRM_IO && !m.writes && !p.owned);
}
int main(void)
{
    lifecycle(); admission(); lost_writes(); switch_failures(); lease_and_restore_faults();
    puts("Ace5 CRM: actual inactive-table writes, channel ACK, preserved resources and timeout rollback passed");
    return 0;
}
