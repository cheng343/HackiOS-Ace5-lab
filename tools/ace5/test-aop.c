/* SPDX-License-Identifier: BSD-2-Clause */
/* Cloud-only: real QMP core, simulated AOP peer and 32-bit MMIO. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "giulia_aop.h"
#define UP GIULIA_AOP_UP
#define DOWN GIULIA_AOP_DOWN
struct mock {
    uint32_t ram[0x400 / 4];
    unsigned reads, writes, kicks, us, stall, message_stall, bad_kick, drop_at;
    unsigned published, last_len;
    unsigned link_writes;
    uint8_t message[0x300];
};
static void init(struct mock *m, unsigned opened)
{
    memset(m, 0, sizeof(*m));
    m->ram[0] = 0x4d41494c; m->ram[1] = 1;
    m->ram[0x34/4] = 0x100; m->ram[0x38/4] = 0x100;
    m->ram[0x0c/4] = UP;
    m->ram[0x10/4] = m->ram[0x14/4] = m->ram[0x18/4] = opened ? UP : DOWN;
    m->ram[0x24/4] = m->ram[0x28/4] = opened ? UP : DOWN;
    m->ram[0x2c/4] = m->ram[0x30/4] = opened ? UP : DOWN;
}
static uint32_t read32(void *cookie, uint32_t off)
{
    struct mock *m = cookie;
    assert(!(off & 3) && off <= sizeof(m->ram) - 4);
    assert(++m->reads < 100000);
    return m->ram[off/4];
}
static void write32(void *cookie, uint32_t off, uint32_t value)
{
    struct mock *m = cookie;
    uint32_t offset = m->ram[0x38/4], size = m->ram[0x34/4];
    assert(!(off & 3) && off <= sizeof(m->ram) - 4);
    assert(off == 0x10 || off == 0x18 || off == 0x24 || off == 0x2c ||
        (off >= offset && off < offset + size));
    if (++m->writes == m->drop_at) return;
    if (off == 0x10 || off == 0x24) ++m->link_writes;
    m->ram[off/4] = value;
    if (off == offset && value) {
        assert(value <= sizeof(m->message));
        ++m->published; m->last_len = value;
        memcpy(m->message, (uint8_t *)m->ram + off + 4, value);
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
    ++m->kicks;
    if (m->bad_kick) return -1;
    if (m->ram[0x2c/4] == DOWN) {
        if (m->stall != 4) {
            m->ram[0x30/4] = DOWN;
            if (m->ram[0x24/4] == DOWN)
                m->ram[0x28/4] = m->ram[0x14/4] = DOWN;
            else if (m->stall != 1) m->ram[0x28/4] = UP;
        }
        return 0;
    }
    if (m->ram[0x24/4] == UP && m->stall != 1) m->ram[0x28/4] = UP;
    if (m->ram[0x2c/4] == UP && m->stall != 2) m->ram[0x14/4] = UP;
    if (m->ram[0x18/4] == UP && m->ram[0x2c/4] == UP && m->stall != 3)
        m->ram[0x30/4] = UP;
    if (!m->message_stall) m->ram[m->ram[0x38/4]/4] = 0;
    return 0;
}
static struct giulia_aop_io io(struct mock *m)
{ return (struct giulia_aop_io){m, read32, write32, barrier, delay, kick}; }
static void borrow(struct mock *m, struct giulia_aop *p)
{
    struct giulia_aop_io v;
    init(m, 1); memset(p, 0, sizeof(*p)); v = io(m);
    assert(giulia_aop_open(p, &v, 0x400, 15, GIULIA_AOP_BORROW_OPEN) == 0);
    assert(!m->writes && !m->kicks && p->borrowed && p->owned);
}
static void admission(void)
{
    struct mock m; struct giulia_aop p = {0};
    struct giulia_aop_io v;
    unsigned i;
    for (i = 0; i < 3; i++) {
        init(&m, 0); v = io(&m);
        assert(giulia_aop_open(&p, &v, 0x400, 7u & ~(1u << i), 0) == GIULIA_AOP_PREREQUISITE);
        assert(!m.reads && !m.writes);
    }
    init(&m, 1); v = io(&m);
    assert(giulia_aop_open(&p, &v, 0x400, 7, 1) == GIULIA_AOP_PREREQUISITE && !m.reads);
    assert(giulia_aop_open(&p, &v, 0x400, 7, 0) == GIULIA_AOP_BUSY && !m.writes);
    for (i = 0; i < 6; i++) {
        init(&m, 0); v = io(&m);
        if (i == 0) m.ram[0] = 0;
        if (i == 1) m.ram[1] = 2;
        if (i == 2) m.ram[0x38/4] = 0x20;
        if (i == 3) m.ram[0x38/4] = 0xfffffffcu;
        if (i == 4) m.ram[0x34/4] = 0x304;
        if (i == 5) m.ram[0x34/4] = 0x101;
        assert(giulia_aop_open(&p, &v, 0x400, 7, 0) == GIULIA_AOP_LAYOUT && !m.writes);
    }
    init(&m, 0); v = io(&m); m.ram[0x100/4] = 64;
    assert(giulia_aop_open(&p, &v, 0x400, 7, 0) == GIULIA_AOP_BUSY && !m.writes);
}
static void cold_lifecycle(void)
{
    struct mock m; struct giulia_aop p = {0}, other = {0};
    struct giulia_aop_io v;
    const char text[] = "{class: wlan_pdc, ss: rf, res: s3c.v, upval: 916}";
    unsigned i;
    init(&m, 0); v = io(&m);
    assert(giulia_aop_open(&p, &v, 0x400, 7, 0) == 0 && p.state == GIULIA_AOP_OPEN);
    assert(m.kicks == 3);
    assert(giulia_aop_open(&other, &v, 0x400, 15, 1) == GIULIA_AOP_BUSY);
    assert(giulia_aop_send_text64(&p, text) == 0);
    assert(m.published == 1 && m.last_len == 64 && !memcmp(m.message, text, sizeof(text)));
    for (i = sizeof(text); i < 64; i++) assert(!m.message[i]);
    assert(giulia_aop_close(&p) == 0 && !p.owned && p.state == GIULIA_AOP_STOPPED);
    assert(m.ram[0x24/4] == DOWN && m.ram[0x2c/4] == DOWN);
    assert(giulia_aop_close(&p) == 0);
}
static void failures(void)
{
    struct mock m; struct giulia_aop p;
    struct giulia_aop_io v;
    unsigned i, writes, kicks;
    uint8_t payload[64];
    memset(payload, 'a', sizeof(payload));
    for (i = 1; i <= 3; i++) {
        init(&m, 0); memset(&p, 0, sizeof(p)); v = io(&m); m.stall = i;
        assert(giulia_aop_open(&p, &v, 0x400, 7, 0) == GIULIA_AOP_TIMEOUT);
        assert(p.state == GIULIA_AOP_FAULT && p.owned && m.us == 1000000);
        m.stall = 0; assert(giulia_aop_close(&p) == 0);
    }
    borrow(&m, &p); m.message_stall = 1;
    assert(giulia_aop_send(&p, payload, 64) == GIULIA_AOP_TIMEOUT);
    assert(p.pending && p.owned && m.published == 1 && m.ram[0x100/4] == 64);
    assert(giulia_aop_close(&p) == GIULIA_AOP_BUSY);
    writes = m.writes; kicks = m.kicks;
    m.ram[0x100/4] = 0; /* AOP eventually consumes it. */
    assert(giulia_aop_recover(&p) == 0 && !p.pending && m.writes == writes && m.kicks == kicks);
    assert(giulia_aop_close(&p) == 0 && m.writes == writes && m.ram[0x24/4] == UP);
    borrow(&m, &p); m.bad_kick = 1;
    assert(giulia_aop_send(&p, payload, 64) == GIULIA_AOP_DOORBELL && p.pending);
    m.bad_kick = 0; m.ram[0x100/4] = 0;
    assert(giulia_aop_recover(&p) == 0 && giulia_aop_close(&p) == 0);
    borrow(&m, &p); m.drop_at = m.writes + 3;
    assert(giulia_aop_send(&p, payload, 64) == GIULIA_AOP_READBACK);
    assert(!m.published && !m.kicks && !p.pending);
    assert(giulia_aop_close(&p) == 0);
    init(&m, 0); memset(&p, 0, sizeof(p)); v = io(&m);
    assert(giulia_aop_open(&p, &v, 0x400, 7, 0) == 0);
    m.stall = 4; assert(giulia_aop_close(&p) == GIULIA_AOP_TIMEOUT && p.owned);
    m.stall = 0; assert(giulia_aop_close(&p) == 0);
}
static void bounds_and_drift(void)
{
    struct mock m; struct giulia_aop p;
    uint8_t data[260] = {0};
    char text[65];
    borrow(&m, &p);
    assert(giulia_aop_send(&p, data, 0) == GIULIA_AOP_ARGUMENT);
    assert(giulia_aop_send(&p, data, 3) == GIULIA_AOP_ARGUMENT);
    assert(giulia_aop_send(&p, data, 256) == GIULIA_AOP_ARGUMENT);
    memset(text, 'x', 64); text[64] = 0;
    assert(giulia_aop_send_text64(&p, text) == GIULIA_AOP_ARGUMENT && !m.writes);
    m.ram[0x38/4] = 0x200;
    assert(giulia_aop_send(&p, data, 4) == GIULIA_AOP_LAYOUT && !m.writes);
    assert(giulia_aop_close(&p) == GIULIA_AOP_LAYOUT && p.owned);
    m.ram[0x38/4] = 0x100; assert(giulia_aop_close(&p) == 0);
}
static void inspect_only(void)
{
    struct mock m; struct giulia_aop_io v;
    struct giulia_aop_snapshot s;
    init(&m, 1); v = io(&m);
    assert(giulia_aop_inspect(&v, 0x400, &s) == 0);
    assert(!s.changed && !s.message_length && m.reads == 31 && !m.writes && !m.kicks);
    m.ram[0x38/4] = 0xfffffffcu;
    assert(giulia_aop_inspect(&v, 0x400, &s) == GIULIA_AOP_LAYOUT);
    assert(m.reads == 46 && !m.writes);
}
static void inherited_link(void)
{
    struct mock m; struct giulia_aop p;
    struct giulia_aop_io v;
    unsigned i;
    for (i = 0; i < 5; i++) {
        init(&m, 1); memset(&p, 0, sizeof(p)); v = io(&m);
        /* Exact descriptor geometry observed on Ace 5, 2026-11-14.
         * APPS link remains up while the local channel is closed. */
        m.ram[0x2c/4] = m.ram[0x30/4] = DOWN;
        m.ram[0x34/4] = 0x2d8; m.ram[0x38/4] = 0x128;
        if (i == 1) m.stall = 3;
        if (i == 2) m.drop_at = 1;
        if (i == 3) m.bad_kick = 1;
        if (i == 4) m.ram[0x28/4] = DOWN;
        int rc = giulia_aop_open(&p, &v, 0x400, 15, GIULIA_AOP_OPEN_ON_INHERITED_LINK);
        if (i == 0) {
            assert(rc == 0 && p.keep_link && !p.borrowed);
            assert(giulia_aop_send_text64(&p, "{class: wlan_pdc, ss: rf, res: s3c.v, upval: 916}") == 0);
            assert(m.published == 1 && m.last_len == 64);
        } else assert(rc != 0);
        m.stall = m.drop_at = m.bad_kick = 0;
        if (p.owned) assert(giulia_aop_close(&p) == 0);
        assert(!m.link_writes && m.ram[0x24/4] == UP && m.ram[0x10/4] == UP);
    }
    puts("Ace5 AOP QMP: inherited live link preserved across channel open, failure and close");
}
int main(void)
{
    admission(); cold_lifecycle(); failures(); bounds_and_drift(); inspect_only(); inherited_link();
    puts("Ace5 AOP QMP: real handshake, publication, timeout retention and borrowed-channel tests passed");
    return 0;
}
