/* SPDX-License-Identifier: BSD-2-Clause */
/* Cloud-only: actual PDC lifecycle + QMP transport, simulated AOP peer.
 * These assertions establish protocol behavior, not radio/PMIC operation. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "giulia_wlan_pdc.h"
#define MO 0x128u
static const uint8_t table[] =
    "{class: wlan_pdc, ss: rf, res: s3c.v, upval: 916}\0"
    "{class: wlan_pdc, ss: rf, res: s3c.v, dwnval: 612}\0"
    "{class: wlan_pdc, ss: rf, res: s1c.v, upval: 1316}\0"
    "{class: wlan_pdc, ss: rf, res: s1c.v, dwnval: 944}\0"
    "{class: wlan_pdc, ss: rf, res: s6c.v, upval: 1864}\0"
    "{class: wlan_pdc, ss: rf, res: s6c.v, dwnval: 1820}\0"
    "{class: wlan_pdc, ss: bb, res: s2c.v, upval: 976}\0"
    "{class: wlan_pdc, ss: bb, res: s2c.v, dwnval: 512}\0"
    "{class: wlan_pdc, ss: bb, res: s4i.v, upval: 940}\0"
    "{class: wlan_pdc, ss: bb, res: s4i.v, dwnval: 420}";
struct mock {
    uint32_t ram[0x400/4];
    unsigned reads, writes, kicks, published, stall_at, drop_at, us;
    char message[64][64];
};
static uint32_t read32(void *cookie, uint32_t off)
{
    struct mock *m = cookie;
    assert(!(off & 3) && off < sizeof(m->ram));
    assert(++m->reads < 200000);
    return m->ram[off/4];
}
static void write32(void *cookie, uint32_t off, uint32_t value)
{
    struct mock *m = cookie;
    assert(!(off & 3) && off >= MO && off <= MO + 64);
    if (++m->writes == m->drop_at) return;
    m->ram[off/4] = value;
    if (off == MO && value) {
        assert(value == 64 && m->published < 64);
        memcpy(m->message[m->published++], (uint8_t *)m->ram + MO + 4, 64);
    }
}
static void barrier(void *cookie) { (void)cookie; }
static void delay(void *cookie, unsigned us)
{
    struct mock *m = cookie;
    assert(us == 100); m->us += us; assert(m->us <= 4000000);
}
static int kick(void *cookie)
{
    struct mock *m = cookie;
    m->kicks++;
    if (m->published != m->stall_at) m->ram[MO/4] = 0;
    return 0;
}
static void setup(struct mock *m, struct giulia_aop *a,
    struct giulia_wlan_pdc_baseline *b)
{
    struct giulia_aop_io io = {m,read32,write32,barrier,delay,kick};
    static const unsigned state[] = {0x0c,0x10,0x14,0x18,0x24,0x28,0x2c,0x30};
    memset(m,0,sizeof(*m)); memset(a,0,sizeof(*a));
    m->ram[0] = 0x4d41494c; m->ram[1] = 1;
    m->ram[0x34/4] = 0x2d8; m->ram[0x38/4] = MO;
    for (unsigned i = 0; i < 8; i++) m->ram[state[i]/4] = GIULIA_AOP_UP;
    b->known_mask = GIULIA_WLAN_PDC_ALL;
    for (unsigned i = 0; i < 10; i++) b->millivolts[i] = i ? 800+i : 0;
    assert(giulia_aop_open(a,&io,0x400,15,GIULIA_AOP_BORROW_OPEN) == 0);
    assert(!m->writes && !m->kicks);
}
static int start(struct giulia_wlan_pdc *p, struct giulia_aop *a,
    const struct giulia_wlan_pdc_baseline *b)
{ return giulia_wlan_pdc_start(p,a,table,sizeof(table),b,3,GIULIA_WLAN_PDC_RESTORE_KNOWN); }
static void check_message(struct mock *m, unsigned at, unsigned index, unsigned value)
{
    const char *expected = (const char *)table;
    char output[64];
    for (unsigned i = 0; i < index; i++) expected += strlen(expected)+1;
    unsigned prefix = (unsigned)(strrchr(expected, ':') + 2 - expected);
    assert(snprintf(output,sizeof(output),"%.*s%u}",(int)prefix,expected,value) > 0);
    assert(!strcmp(m->message[at],output));
    for (unsigned i = (unsigned)strlen(output)+1; i < 64; i++) assert(!m->message[at][i]);
}
static void admission(void)
{
    struct mock m; struct giulia_aop a; struct giulia_wlan_pdc p = {0};
    struct giulia_wlan_pdc_baseline b;
    uint8_t altered[sizeof(table)+1];
    setup(&m,&a,&b);
    for (unsigned i = 0; i < 2; i++)
        assert(giulia_wlan_pdc_start(&p,&a,table,sizeof(table),&b,3u & ~(1u<<i),0)
            == GIULIA_AOP_PREREQUISITE);
    assert(start(&p,&a,NULL) == GIULIA_WLAN_PDC_BASELINE_UNKNOWN);
    b.known_mask &= ~8u;
    assert(start(&p,&a,&b) == GIULIA_WLAN_PDC_BASELINE_UNKNOWN);
    b.known_mask = GIULIA_WLAN_PDC_ALL; b.millivolts[2] = 8192;
    assert(start(&p,&a,&b) == GIULIA_AOP_ARGUMENT); b.millivolts[2] = 802;
    memcpy(altered,table,sizeof(table)); altered[sizeof(table)] = 0;
    for (unsigned i = 0; i < sizeof(table); i++)
        assert(giulia_wlan_pdc_start(&p,&a,table,i,&b,3,0) == GIULIA_WLAN_PDC_TABLE_MISMATCH);
    assert(giulia_wlan_pdc_start(&p,&a,altered,sizeof(altered),&b,3,0)
        == GIULIA_WLAN_PDC_TABLE_MISMATCH);
    altered[42] ^= 1;
    assert(giulia_wlan_pdc_start(&p,&a,altered,sizeof(table),&b,3,0)
        == GIULIA_WLAN_PDC_TABLE_MISMATCH);
    assert(!m.writes && !m.kicks && sizeof(table) == 508);
    assert(giulia_aop_close(&a) == 0);
}
static void lifecycle(void)
{
    struct mock m; struct giulia_aop a; struct giulia_wlan_pdc p = {0}, other = {0};
    struct giulia_wlan_pdc_baseline b;
    static const unsigned target[] = {916,612,1316,944,1864,1820,976,512,940,420};
    setup(&m,&a,&b);
    assert(start(&p,&a,&b) == 0 && p.state == GIULIA_WLAN_PDC_DELIVERED);
    assert(p.attempted == 0x3ff && p.delivered == 0x3ff && m.published == 10);
    assert(start(&other,&a,&b) == GIULIA_AOP_BUSY);
    assert(giulia_wlan_pdc_stop(&p,1) == GIULIA_AOP_PREREQUISITE && m.published == 10);
    assert(giulia_wlan_pdc_stop(&p,3) == 0 && m.published == 20 && !p.attempted);
    for (unsigned i = 0; i < 10; i++) {
        check_message(&m,i,i,target[i]);
        check_message(&m,10+i,9-i,b.millivolts[9-i]);
    }
    assert(giulia_wlan_pdc_stop(&p,3) == 0 && m.published == 20);
    assert(start(&p,&a,&b) == 0 && giulia_wlan_pdc_stop(&p,3) == 0);
    assert(giulia_aop_close(&a) == 0);
}
static void partial_delivery(void)
{
    const unsigned failures[] = {1,5,10};
    for (unsigned i = 0; i < 3; i++) {
        struct mock m; struct giulia_aop a; struct giulia_wlan_pdc p = {0};
        struct giulia_wlan_pdc_baseline b;
        unsigned n = failures[i];
        setup(&m,&a,&b); m.stall_at = n;
        assert(start(&p,&a,&b) == GIULIA_AOP_TIMEOUT && m.published == n);
        assert(p.attempted == (1u<<n)-1 && p.delivered == (1u<<(n-1))-1);
        unsigned writes = m.writes;
        assert(giulia_wlan_pdc_recover(&p) == GIULIA_AOP_TIMEOUT);
        assert(m.writes == writes && m.published == n);
        m.ram[MO/4] = 0; /* Late ACK; recovery must not resend. */
        assert(giulia_wlan_pdc_recover(&p) == 0 && m.writes == writes);
        assert(p.delivered == (1u<<n)-1);
        assert(giulia_wlan_pdc_stop(&p,3) == 0 && m.published == 2*n);
        for (unsigned j = 0; j < n; j++) check_message(&m,n+j,n-1-j,b.millivolts[n-1-j]);
        assert(giulia_aop_close(&a) == 0);
    }
}
static void partial_restore(void)
{
    struct mock m; struct giulia_aop a; struct giulia_wlan_pdc p = {0};
    struct giulia_wlan_pdc_baseline b;
    setup(&m,&a,&b);
    assert(start(&p,&a,&b) == 0); m.stall_at = 13;
    assert(giulia_wlan_pdc_stop(&p,3) == GIULIA_AOP_TIMEOUT && m.published == 13);
    assert(p.attempted == 0xff && p.restoring && p.pending_index == 7);
    m.ram[MO/4] = 0;
    assert(giulia_wlan_pdc_recover(&p) == 0 && p.attempted == 0x7f);
    assert(giulia_wlan_pdc_stop(&p,3) == 0 && m.published == 20);
    for (unsigned i = 0; i < 10; i++) check_message(&m,10+i,9-i,b.millivolts[9-i]);
    assert(giulia_aop_close(&a) == 0);
}
static void prepublish_failure(void)
{
    struct mock m; struct giulia_aop a; struct giulia_wlan_pdc p = {0};
    struct giulia_wlan_pdc_baseline b;
    setup(&m,&a,&b); m.drop_at = 1;
    assert(start(&p,&a,&b) == GIULIA_AOP_READBACK);
    assert(!p.attempted && !m.published && !a.pending);
    assert(giulia_wlan_pdc_stop(&p,3) == GIULIA_AOP_STATE);
    /* Repair transport only; no message was published to compensate. */
    assert(giulia_aop_close(&a) == 0);
    struct giulia_aop_io io = {&m,read32,write32,barrier,delay,kick};
    assert(giulia_aop_open(&a,&io,0x400,15,GIULIA_AOP_BORROW_OPEN) == 0);
    assert(giulia_wlan_pdc_stop(&p,3) == 0 && !m.published);
    assert(giulia_aop_close(&a) == 0);
}
static void forward_only_last(void)
{
    static struct mock m; static struct giulia_aop a;
    static struct giulia_wlan_pdc p;
    struct giulia_wlan_pdc other = {0}; struct giulia_wlan_pdc_baseline b;
    setup(&m,&a,&b);
    assert(giulia_wlan_pdc_start(&p,&a,table,sizeof(table),NULL,3,GIULIA_WLAN_PDC_KEEP_TABLE) == 0);
    assert(giulia_wlan_pdc_stop(&p,3) == GIULIA_WLAN_PDC_RETAINED);
    assert(p.state == GIULIA_WLAN_PDC_HELD && m.published == 10);
    assert(giulia_wlan_pdc_stop(&p,3) == GIULIA_WLAN_PDC_RETAINED && m.published == 10);
    assert(start(&other,&a,&b) == GIULIA_AOP_BUSY);
    assert(giulia_aop_close(&a) == 0);
}
int main(void)
{
    assert(!giulia_wlan_pdc_check(table,sizeof(table),NULL,GIULIA_WLAN_PDC_KEEP_TABLE));
    assert(giulia_wlan_pdc_check(table,sizeof(table),NULL,GIULIA_WLAN_PDC_RESTORE_KNOWN)==GIULIA_WLAN_PDC_BASELINE_UNKNOWN);
    for(unsigned n=0;n<sizeof(table);n++)
        assert(giulia_wlan_pdc_check(table,n,NULL,GIULIA_WLAN_PDC_KEEP_TABLE)==GIULIA_WLAN_PDC_TABLE_MISMATCH);
    admission(); lifecycle(); partial_delivery(); partial_restore();
    prepublish_failure(); forward_only_last();
    puts("Ace5 WLAN PDC: actual ordered messages, strict baseline, late ACK and partial restore passed");
    return 0;
}
