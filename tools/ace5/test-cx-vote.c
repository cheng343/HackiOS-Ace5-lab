/* SPDX-License-Identifier: BSD-2-Clause */
/* Actual QMP + CX capture cores, synthetic firmware and SRAM. */
#define main original_aop_tests
#include "test-aop.c"
#undef main
#include "giulia_cx_vote.h"
struct stats {
    struct mock *aop;
    uint32_t words[7];
    unsigned reads,fail,change;
};
/* Independent golden bytes for the decoded firmware mask 0x0180f800. */
static const uint8_t fixture[24]={0,1,2,3,0,1,2,3,0,1,2,0xfe,
    0xfe,0xfe,0xfe,0xfe,0,1,2,3,0,1,2,0xfe};
static int stats_read(void *cookie,uint32_t offset,uint32_t *value)
{
    struct stats *s=cookie;
    unsigned n=s->reads++;
    assert(n<14 && offset==0x1f0+(n<7 ? n:13-n)*4);
    assert(s->aop->published==1 && s->aop->ram[0x100/4]==0);
    assert(s->aop->last_len==64 && !strcmp((char *)s->aop->message,"{class: misc_debug, res: cx_vote}"));
    for (unsigned i=34;i<64;i++) assert(!s->aop->message[i]);
    if (s->reads==s->fail) return -81;
    *value=s->words[(offset-0x1f0)/4] ^ (s->reads==s->change ? 0x10000000u:0);
    return 0;
}
static struct giulia_cx_vote_io prepare(struct stats *s,struct mock *m,struct giulia_aop *p)
{
    borrow(m,p); *s=(struct stats){.aop=m};
    for (unsigned i=0;i<24;i++) {
        unsigned v=fixture[i];
        s->words[i/4]|=v<<((i%4)*8);
    }
    s->words[6]=0x7f7f7f7f;
    return (struct giulia_cx_vote_io){s,stats_read,GIULIA_CX_STATS_PHYS,GIULIA_CX_STATS_SIZE};
}
int main(void)
{
    struct stats s; struct mock m; struct giulia_aop p;
    struct giulia_cx_vote result;
    struct giulia_arc_resource cx={.address=0x30000,.count=4,.level={0,16,64,128}};
    struct giulia_cx_vote_io v=prepare(&s,&m,&p);
    assert(!giulia_cx_vote_capture(&p,&v,&cx,&result));
    assert(result.valid && result.reads==14 && result.selector[2]==2 && m.published==1);
    assert(!giulia_aop_close(&p));
    for (unsigned n=1;n<=14;n++) for (unsigned kind=0;kind<2;kind++) {
        v=prepare(&s,&m,&p); if (kind) s.change=n; else s.fail=n;
        int rc=giulia_cx_vote_capture(&p,&v,&cx,&result);
        assert(rc==(kind ? GIULIA_CX_VOTE_CHANGED:GIULIA_CX_VOTE_IO));
        assert(!result.valid && result.reads==(kind ? 14u:n));
        assert(!p.pending && m.published==1 && !giulia_aop_close(&p));
    }
    for (unsigned row=0;row<25;row++) {
        v=prepare(&s,&m,&p);
        if (row==24) s.words[6]=0;
        else {
            unsigned shift=(row%4)*8,invalid=fixture[row]==0xfe ? 0:4;
            s.words[row/4]=(s.words[row/4]&~(255u<<shift)) | (invalid<<shift);
        }
        assert(giulia_cx_vote_capture(&p,&v,&cx,&result)==GIULIA_CX_VOTE_VALUE && !result.valid);
        assert(!giulia_aop_close(&p));
    }
    for (unsigned bad=0;bad<4;bad++) {
        v=prepare(&s,&m,&p); struct giulia_arc_resource c=cx;
        if (bad==0) v.phys+=4;
        if (bad==1) v.size-=4;
        if (bad==2) c.address+=16;
        if (bad==3) c.count=17;
        assert(giulia_cx_vote_capture(&p,&v,&c,&result)==GIULIA_CX_VOTE_LAYOUT);
        assert(!s.reads && !m.published && !giulia_aop_close(&p));
    }
    v=prepare(&s,&m,&p); m.message_stall=1;
    assert(giulia_cx_vote_capture(&p,&v,&cx,&result)==GIULIA_AOP_TIMEOUT);
    assert(p.pending && p.owned && !s.reads && m.published==1);
    assert(giulia_cx_vote_capture(&p,&v,&cx,&result)==GIULIA_AOP_STATE && m.published==1 && !s.reads);
    m.ram[0x100/4]=0;
    assert(!giulia_aop_recover(&p) && !giulia_aop_close(&p));
    puts("Ace5 CX AOP: fixed diagnostic, ACK before SRAM, all read/drift faults and malformed output rejected");
    puts("Ace5 CX AOP: pending query retains real QMP lease and is never resent");
    return 0;
}
