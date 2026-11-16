/* SPDX-License-Identifier: BSD-2-Clause */
/* Link with the real giulia_pcie_power.c; cloud-only native fixture. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "giulia_pcie_power.h"

#define BIT(n) (1u << (n))
#define CFG_MASK 0x10371fu
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
struct reg { unsigned block; uint32_t off, value, original; };
struct event { unsigned block; uint32_t off, value; unsigned us; };
struct mock {
    struct reg regs[32];
    struct event events[512];
    unsigned count, reads, writes, barriers, us;
    unsigned drop_at, gdsc_stuck, other_master_on;
    uint32_t root_stuck, update_stuck, branch_stuck;
    uint32_t all_ones_off;
    unsigned all_ones_after;
    unsigned blocked;
    unsigned cold_down, preserve_rchng;
    uint32_t block_off, block_mask;
    uint32_t active_aux, active_rch;
};

static struct reg *find(struct mock *m, unsigned block, uint32_t off)
{
    unsigned i;
    for (i = 0; i < m->count; i++)
        if (m->regs[i].block == block && m->regs[i].off == off)
            return &m->regs[i];
    assert(!"unexpected MMIO address");
    return NULL;
}
static uint32_t raw(struct mock *m, unsigned block, uint32_t off)
{
    return find(m, block, off)->value;
}
static void seed(struct mock *m, unsigned block, uint32_t off, uint32_t v)
{
    struct reg *r;
    assert(m->count < COUNT(m->regs));
    r = &m->regs[m->count++];
    r->block = block; r->off = off; r->value = r->original = v;
}
static void init(struct mock *m)
{
    memset(m, 0, sizeof(*m));
    seed(m, 0, 0x6b004, 0x80004008u);
    seed(m, 0, 0x6c000, 0x40009008u);
    seed(m, 0, 0x5214c, BIT(0) | BIT(3) | BIT(7));
    seed(m, 0, 0x52000, BIT(12) | BIT(24)); /* inherited shared NoC */
    seed(m, 0, 0x52008, BIT(6) | BIT(20));
    seed(m, 1, 0x31100, BIT(0) | BIT(7)); /* inherited ref */
    seed(m, 0, 0x6b070, 0x100u);
    seed(m, 0, 0x6b078, 0x40003507u);
    seed(m, 0, 0x6b074, BIT(1) | BIT(25)); /* inherited root force */
    seed(m, 0, 0x6b05c, 0x20000001u);
    seed(m, 0, 0x6b058, BIT(25));
    seed(m, 0, 0x6b000, BIT(0) | BIT(10));
    seed(m, 0, 0x6c01c, BIT(10));
    seed(m, 0, 0x6b03c, 0); /* AUX */
    seed(m, 0, 0x6b038, BIT(31) | BIT(1)); /* HWCG may look OFF */
    seed(m, 0, 0x6b02c, BIT(31)); /* HALT_SKIP master */
    seed(m, 0, 0x6b020, BIT(31) | (2u << 28)); /* FSM ON */
    seed(m, 0, 0x6b01c, 0);
    seed(m, 0, 0x10074, BIT(31)); /* HALT_SKIP DDRSS */
    seed(m, 0, 0x10064, BIT(31)); /* HALT_SKIP aggregate */
    seed(m, 0, 0x10058, 0);
    seed(m, 0, 0x6b054, 0);
    m->active_aux = raw(m, 0, 0x6b078) & CFG_MASK;
    m->active_rch = raw(m, 0, 0x6b05c) & CFG_MASK;
}

static uint32_t read32(void *cookie, unsigned block, uint32_t off)
{
    struct mock *m = cookie;
    struct reg *r;
    m->reads++;
    assert(m->reads < 30000); /* Every failure path must be bounded. */
    if (block == 0 && m->all_ones_off == off &&
        m->writes >= m->all_ones_after)
        return 0xffffffffu;
    if (block == 0 && (off == 0x6b008 || off == 0x6c004)) {
        unsigned vote = off == 0x6b008 ? BIT(0) : BIT(3);
        if (m->other_master_on & vote)
            return BIT(16);
        return !(raw(m, 0, 0x5214c) & vote) && !(m->gdsc_stuck & vote)
            ? BIT(16) : (m->cold_down ? BIT(15) : 0);
    }
    r = find(m, block, off);
    if (block == 0 && (off == 0x6b074 || off == 0x6b058)) {
        if ((r->value & BIT(0)) && m->update_stuck != off) {
            r->value &= ~BIT(0);
            if (off == 0x6b074)
                m->active_aux = raw(m, 0, 0x6b078) & CFG_MASK;
            else
                m->active_rch = raw(m, 0, 0x6b05c) & CFG_MASK;
        }
        if (m->root_stuck == off)
            return r->value | BIT(31);
    }
    if (block == 0 && m->branch_stuck == off)
        return BIT(31);
    return r->value;
}
static void write32(void *cookie, unsigned block, uint32_t off, uint32_t v)
{
    struct mock *m = cookie;
    struct reg *r = find(m, block, off);
    struct event *e;
    assert(m->writes < COUNT(m->events));
    e = &m->events[m->writes++];
    e->block = block; e->off = off; e->value = v; e->us = m->us;
    /* Only the 13 explicitly owned registers may be written. No CBCR, PHY,
     * CFG status, PLL, GPIO, firmware vote or collateral clock writes.
     */
    assert((unsigned)(r - m->regs) < 13);
    if (m->preserve_rchng && block == 0 && off == 0x52000)
        assert(v & BIT(22));
    if (m->writes == m->drop_at)
        return;
    if (m->blocked && block == 0 && off == m->block_off)
        v = (v & ~m->block_mask) | (r->value & m->block_mask);
    if (block == 0 && (off == 0x6b074 || off == 0x6b058))
        v &= ~BIT(31); /* ROOT_OFF is status, not writable storage. */
    r->value = v;
}
static void barrier(void *cookie) { ((struct mock *)cookie)->barriers++; }
static void wait_us(void *cookie, unsigned us)
{
    struct mock *m = cookie;
    assert(us && us <= 1500);
    m->us += us;
    assert(m->us < 40000);
}
static struct giulia_pcie_power_io io(struct mock *m)
{
    struct giulia_pcie_power_io v = { m, read32, write32, barrier, wait_us };
    return v;
}
static void restored(struct mock *m, struct giulia_pcie_power *p)
{
    unsigned i;
    assert(p->state == GIULIA_POWER_STOPPED && p->dirty == 0);
    for (i = 0; i < 13; i++)
        assert(m->regs[i].value == m->regs[i].original);
    assert(m->active_aux == (m->regs[7].original & CFG_MASK));
    assert(m->active_rch == (m->regs[9].original & CFG_MASK));
    assert(m->barriers == m->writes);
}
static unsigned event_at(struct mock *m, uint32_t off, uint32_t mask,
    uint32_t value, unsigned after)
{
    unsigned i;
    for (i = after; i < m->writes; i++)
        if (m->events[i].off == off &&
            (m->events[i].value & mask) == value)
            return i;
    assert(!"required event missing");
    return 0;
}

static void lifecycle(void)
{
    struct mock m;
    struct giulia_pcie_power p = {0};
    struct giulia_pcie_power_io v;
    unsigned core_assert, core_release, phy_assert, phy_release;
    unsigned core_vote, core_retain, count;
    init(&m); v = io(&m);
    assert(giulia_pcie_power_start(&p, &v, GIULIA_POWER_REQUIRED) == 0);
    assert(p.state == GIULIA_POWER_READY);
    assert((raw(&m, 0, 0x52008) & 0x7fu) == 0x6fu);
    assert((raw(&m, 0, 0x52000) & (BIT(12) | BIT(19) | BIT(22))) ==
        (BIT(12) | BIT(19) | BIT(22)));
    assert(m.active_aux == 1 && m.active_rch == 0x605);
    assert((raw(&m, 0, 0x6b074) & BIT(1)) != 0);
    assert((raw(&m, 0, 0x6b058) & BIT(1)) == 0);
    core_vote = event_at(&m, 0x5214c, BIT(0), 0, 0);
    core_retain = event_at(&m, 0x6b004, BIT(11), BIT(11), 0);
    assert(m.events[core_retain].us >= m.events[core_vote].us + 1);
    core_assert = event_at(&m, 0x6b000, 1, 1, 0);
    core_release = event_at(&m, 0x6b000, 1, 0, core_assert + 1);
    phy_assert = event_at(&m, 0x6c01c, 1, 1, core_release + 1);
    phy_release = event_at(&m, 0x6c01c, 1, 0, phy_assert + 1);
    assert(m.events[core_release].us >= m.events[core_assert].us + 1000);
    assert(m.events[phy_assert].us >= m.events[core_release].us + 1000);
    assert(m.events[phy_release].us >= m.events[phy_assert].us + 200);
    count = m.writes;
    assert(giulia_pcie_power_stop(&p, 0) == GIULIA_POWER_PREREQUISITE);
    assert(m.writes == count);
    assert(giulia_pcie_power_pipe_enable(&p) == 0);
    assert(p.state == GIULIA_POWER_PIPE_ON);
    assert((raw(&m, 0, 0x6b070) & 3) == 0);
    assert(raw(&m, 0, 0x52008) & BIT(4));
    assert(giulia_pcie_power_pipe_disable(&p) == 0);
    assert(p.state == GIULIA_POWER_READY);
    assert(!(raw(&m, 0, 0x52008) & BIT(4)));
    assert((raw(&m, 0, 0x6b070) & 3) == 2);
    /* PHY control writes must still have aux/ref/rchng after PIPE stops. */
    assert(raw(&m, 0, 0x52008) & BIT(3));
    assert(raw(&m, 0, 0x52000) & BIT(22));
    assert(raw(&m, 1, 0x31100) & 1);
    /* Another owner changes unrelated vote bits while our resources are on. */
    find(&m, 0, 0x52000)->value |= BIT(27);
    find(&m, 0, 0x52000)->original |= BIT(27);
    m.other_master_on = BIT(0) | BIT(3);
    assert(giulia_pcie_power_stop(&p, GIULIA_POWER_STOP_REQUIRED) == 0);
    restored(&m, &p);
    count = m.reads;
    assert(giulia_pcie_power_stop(&p, 0) == 0 && m.reads == count);
}

static void admission(void)
{
    struct mock m;
    struct giulia_pcie_power p;
    struct giulia_pcie_power_io v;
    unsigned i;
    const uint32_t offs[] = {0x52008, 0x52000, 0x6b004, 0x6c000,
        0x6b070, 0x6b074, 0x6b058};
    const uint32_t bits[] = {1, BIT(22), 2, 4, 1, 1, 1};
    for (i = 0; i < 8; i++) {
        init(&m); memset(&p, 0, sizeof(p)); v = io(&m);
        assert(giulia_pcie_power_start(&p, &v,
            GIULIA_POWER_REQUIRED & ~BIT(i)) == GIULIA_POWER_PREREQUISITE);
        assert(m.reads == 0 && m.writes == 0);
    }
    for (i = 0; i < COUNT(offs); i++) {
        init(&m); memset(&p, 0, sizeof(p)); v = io(&m);
        find(&m, 0, offs[i])->value |= bits[i];
        if (offs[i] == 0x6b074 || offs[i] == 0x6b058)
            m.update_stuck = offs[i];
        assert(giulia_pcie_power_start(&p, &v, GIULIA_POWER_REQUIRED) ==
            GIULIA_POWER_BUSY);
        assert(m.writes == 0 && p.state == GIULIA_POWER_IDLE);
    }
}

static void inherited_init(struct mock *m)
{
    struct reg *r;
    init(m); m->cold_down = m->preserve_rchng = 1;
    r = find(m, 0, 0x52000); r->value |= BIT(22); r->original = r->value;
    r = find(m, 0, 0x6b004); r->value = r->original = 0x22f801;
    r = find(m, 0, 0x6c000); r->value = r->original = 0x222801;
    r = find(m, 0, 0x6b05c); r->value = r->original = 0x100000;
    m->active_rch = 0x100000;
}
static void inherited_rchng(void)
{
    struct mock m;
    struct giulia_pcie_power p;
    struct giulia_pcie_power_io v;
    unsigned i, limit;
    const unsigned flags = GIULIA_POWER_REQUIRED | GIULIA_POWER_RCHNG_HANDOFF;
    inherited_init(&m); memset(&p, 0, sizeof(p)); v = io(&m);
    assert(giulia_pcie_power_start(&p, &v, flags) == 0 && p.inherited_rchng);
    assert(m.active_rch == 0x605 && (raw(&m, 0, 0x52000) & BIT(22)));
    limit = m.writes;
    assert(giulia_pcie_power_stop(&p, GIULIA_POWER_STOP_REQUIRED) == 0);
    restored(&m, &p);
    /* A real cold-domain status and an explicit lease are both required. */
    for (i = 0; i < 7; i++) {
        unsigned f = flags;
        inherited_init(&m); memset(&p, 0, sizeof(p)); v = io(&m);
        if (i == 0) f &= ~GIULIA_POWER_RCHNG_HANDOFF;
        if (i == 1) m.cold_down = 0;
        if (i == 2) m.other_master_on = BIT(0);
        if (i == 3) find(&m, 0, 0x6c000)->value &= ~1u;
        if (i == 4) find(&m, 0, 0x5214c)->value &= ~BIT(3);
        if (i == 5) find(&m, 0, 0x52008)->value |= BIT(3);
        if (i == 6) m.branch_stuck = 0x6b054;
        assert(giulia_pcie_power_start(&p, &v, f) == GIULIA_POWER_BUSY);
        assert(!m.writes && p.state == GIULIA_POWER_IDLE);
    }
    inherited_init(&m); memset(&p, 0, sizeof(p)); v = io(&m);
    m.all_ones_off = 0x6b008;
    assert(giulia_pcie_power_start(&p, &v, flags) == GIULIA_POWER_READBACK && !m.writes);
    /* Every start write and rollback must keep the inherited vote set. */
    for (i = 1; i <= limit; i++) {
        int rc;
        inherited_init(&m); memset(&p, 0, sizeof(p)); v = io(&m); m.drop_at = i;
        rc = giulia_pcie_power_start(&p, &v, flags);
        assert(rc == 0 || rc == GIULIA_POWER_READBACK || rc == GIULIA_POWER_TIMEOUT);
        m.drop_at = 0;
        assert(giulia_pcie_power_stop(&p, GIULIA_POWER_STOP_REQUIRED) == 0);
        restored(&m, &p);
    }
    inherited_init(&m); memset(&p, 0, sizeof(p)); v = io(&m);
    m.update_stuck = 0x6b058;
    assert(giulia_pcie_power_start(&p, &v, flags) == GIULIA_POWER_TIMEOUT);
    assert(p.state == GIULIA_POWER_FAULT && (raw(&m, 0, 0x52000) & BIT(22)));
    m.update_stuck = 0;
    assert(giulia_pcie_power_stop(&p, GIULIA_POWER_STOP_REQUIRED) == 0);
    restored(&m, &p);
    puts("SM8650 inherited RCHNG: cold handoff, preserved vote and rollback passed");
}

static void timeouts(void)
{
    struct mock m;
    struct giulia_pcie_power p;
    struct giulia_pcie_power_io v;
    unsigned i;
    /* GDSC and branch failures can unwind immediately. */
    for (i = 0; i < 4; i++) {
        init(&m); memset(&p, 0, sizeof(p)); v = io(&m);
        if (i < 2) m.gdsc_stuck = i == 0 ? BIT(0) : BIT(3);
        else m.branch_stuck = i == 2 ? 0x6b03c : 0x6b054;
        assert(giulia_pcie_power_start(&p, &v, GIULIA_POWER_REQUIRED) ==
            GIULIA_POWER_TIMEOUT);
        assert(p.last_error == GIULIA_POWER_TIMEOUT && p.cleanup_error == 0);
        restored(&m, &p);
    }
    /* A hung root/update cannot be falsely restored by writing the shadow.
     * Retain resources and saved state until the hardware responds on retry.
     */
    for (i = 0; i < 4; i++) {
        init(&m); memset(&p, 0, sizeof(p)); v = io(&m);
        if (i < 2) m.root_stuck = i == 0 ? 0x6b074 : 0x6b058;
        else m.update_stuck = i == 2 ? 0x6b074 : 0x6b058;
        assert(giulia_pcie_power_start(&p, &v, GIULIA_POWER_REQUIRED) ==
            GIULIA_POWER_TIMEOUT);
        assert(p.state == GIULIA_POWER_FAULT && p.dirty != 0);
        assert(p.cleanup_error == GIULIA_POWER_TIMEOUT);
        assert((raw(&m, 0, 0x5214c) & 9u) == 0);
        m.root_stuck = m.update_stuck = 0;
        assert(giulia_pcie_power_stop(&p, GIULIA_POWER_STOP_REQUIRED) == 0);
        restored(&m, &p);
    }
}

static void readback_and_retry(void)
{
    struct mock m;
    struct giulia_pcie_power p = {0};
    struct giulia_pcie_power_io v;
    unsigned limit, n;
    init(&m); v = io(&m);
    assert(giulia_pcie_power_start(&p, &v, GIULIA_POWER_REQUIRED) == 0);
    limit = m.writes;
    assert(giulia_pcie_power_stop(&p, GIULIA_POWER_STOP_REQUIRED) == 0);
    /* Drop each stable-field start write once. Any detected partial write
     * must unwind; a redundant write can validly succeed. Self-clearing
     * UPDATE cannot distinguish an ignored write from completion by readback
     * alone; this test requires the final committed configuration restored.
     */
    for (n = 1; n <= limit; n++) {
        int r;
        init(&m); memset(&p, 0, sizeof(p)); v = io(&m); m.drop_at = n;
        r = giulia_pcie_power_start(&p, &v, GIULIA_POWER_REQUIRED);
        if (r == 0)
            assert(giulia_pcie_power_stop(&p, GIULIA_POWER_STOP_REQUIRED) == 0);
        else {
            assert(r == GIULIA_POWER_READBACK || r == GIULIA_POWER_TIMEOUT);
            if (p.state == GIULIA_POWER_FAULT) {
                m.drop_at = 0;
                assert(giulia_pcie_power_stop(&p, GIULIA_POWER_STOP_REQUIRED) == 0);
            }
        }
        restored(&m, &p);
    }
    init(&m); memset(&p, 0, sizeof(p)); v = io(&m);
    assert(giulia_pcie_power_start(&p, &v, GIULIA_POWER_REQUIRED) == 0);
    m.blocked = 1; m.block_off = 0x52008; m.block_mask = BIT(4);
    assert(giulia_pcie_power_pipe_enable(&p) == GIULIA_POWER_READBACK);
    assert(p.state == GIULIA_POWER_FAULT);
    m.blocked = 0;
    assert(giulia_pcie_power_pipe_disable(&p) == 0);
    assert(p.state == GIULIA_POWER_FAULT); /* Do not hide an earlier fault. */
    assert(giulia_pcie_power_stop(&p, GIULIA_POWER_STOP_REQUIRED) == 0);
    restored(&m, &p);

    init(&m); memset(&p, 0, sizeof(p)); v = io(&m);
    assert(giulia_pcie_power_start(&p, &v, GIULIA_POWER_REQUIRED) == 0);
    assert(giulia_pcie_power_pipe_enable(&p) == 0);
    m.blocked = 1; m.block_off = 0x52008; m.block_mask = BIT(4);
    assert(giulia_pcie_power_stop(&p, GIULIA_POWER_STOP_REQUIRED) ==
        GIULIA_POWER_READBACK);
    assert(p.state == GIULIA_POWER_FAULT);
    assert(raw(&m, 0, 0x52008) & BIT(3)); /* Keep PHY access alive. */
    m.blocked = 0;
    assert(giulia_pcie_power_stop(&p, GIULIA_POWER_STOP_REQUIRED) == 0);
    restored(&m, &p);
}

static void inaccessible_mmio(void)
{
    struct mock m;
    struct giulia_pcie_power p;
    struct giulia_pcie_power_io v;
    const uint32_t offsets[] = {0x6b004, 0x6b008, 0x6c004, 0x6b038,
        0x6b074, 0x6b078};
    unsigned i;
    for (i = 0; i < COUNT(offsets); i++) {
        init(&m); memset(&p, 0, sizeof(p)); v = io(&m);
        m.all_ones_off = offsets[i];
        /* AUX CFG is readable for the initial snapshot, then disappears. */
        m.all_ones_after = offsets[i] == 0x6b078 ? 1 : 0;
        assert(giulia_pcie_power_start(&p, &v, GIULIA_POWER_REQUIRED) ==
            GIULIA_POWER_READBACK);
        if (p.state == GIULIA_POWER_IDLE) {
            assert(m.writes == 0);
        } else {
            m.all_ones_off = 0;
            assert(giulia_pcie_power_stop(&p, GIULIA_POWER_STOP_REQUIRED) == 0);
            restored(&m, &p);
        }
    }
    /* All-ones after a write is also failure, even where the requested
     * mask consists entirely of ones (e.g. PIPE enable).
     */
    init(&m); memset(&p, 0, sizeof(p)); v = io(&m);
    assert(giulia_pcie_power_start(&p, &v, GIULIA_POWER_REQUIRED) == 0);
    m.all_ones_off = 0x52008;
    m.all_ones_after = m.writes + 2;
    assert(giulia_pcie_power_pipe_enable(&p) == GIULIA_POWER_READBACK);
    assert(p.state == GIULIA_POWER_FAULT);
    m.all_ones_off = 0;
    assert(giulia_pcie_power_stop(&p, GIULIA_POWER_STOP_REQUIRED) == 0);
    restored(&m, &p);
}

static void staged_cesta(void)
{
    struct mock m;
    struct giulia_pcie_power p;
    struct giulia_pcie_power_io v;
    const uint32_t flags = GIULIA_POWER_HOST_REQUIRED | GIULIA_POWER_CESTA_COORDINATED;
    const uint32_t finish = flags | GIULIA_POWER_NOC_READY;
    const uint32_t stop = GIULIA_POWER_STOP_REQUIRED | GIULIA_POWER_STOP_CESTA_RESOURCES_QUIET;
    unsigned prepared_writes, finish_writes, before;
    init(&m); memset(&p, 0, sizeof(p)); v = io(&m);
    assert(giulia_pcie_power_start(&p, &v, finish) == GIULIA_POWER_PREREQUISITE);
    assert(!m.reads && !m.writes); /* The old API cannot bypass the split. */
    assert(!giulia_pcie_power_prepare_host(&p, &v, flags));
    assert(p.state == GIULIA_POWER_HOST_PREPARED && p.dirty);
    assert(!p.reset_attempted);
    assert((raw(&m,0,0x52008) & 0x7f) == 0x6f);
    for (unsigned i = 0; i < m.writes; i++)
        assert(m.events[i].off != 0x6b000 && m.events[i].off != 0x6c01c &&
               !(m.events[i].off == 0x52008 && (m.events[i].value & BIT(4))));
    prepared_writes = m.writes; before = m.reads;
    assert(giulia_pcie_power_pipe_enable(&p) == GIULIA_POWER_STATE);
    assert(giulia_pcie_power_finish_reset(&p, flags) == GIULIA_POWER_PREREQUISITE);
    assert(giulia_pcie_power_finish_reset(&p, GIULIA_POWER_REQUIRED) == GIULIA_POWER_PREREQUISITE);
    assert(giulia_pcie_power_finish_reset(&p, finish | GIULIA_POWER_DIRECT_VOTES) == GIULIA_POWER_PREREQUISITE);
    for (unsigned i = 0; i < 10; i++) {
        if (!(finish & BIT(i))) continue;
        assert(giulia_pcie_power_finish_reset(&p, finish & ~BIT(i)) == GIULIA_POWER_PREREQUISITE);
    }
    assert(m.reads == before && m.writes == prepared_writes && p.state == GIULIA_POWER_HOST_PREPARED);
    assert(!p.reset_attempted);
    /* The platform resource service runs here, with host clocks still on.
     * This fixture supplies a completed service contract, not hardware proof. */
    assert(!giulia_pcie_power_finish_reset(&p, finish) && p.state == GIULIA_POWER_READY);
    assert(p.reset_attempted);
    assert(event_at(&m,0x6b000,1,1,prepared_writes) >= prepared_writes);
    finish_writes = m.writes - prepared_writes;
    before = m.writes;
    assert(giulia_pcie_power_stop(&p, GIULIA_POWER_STOP_REQUIRED) == GIULIA_POWER_PREREQUISITE);
    assert(m.writes == before && p.state == GIULIA_POWER_READY);
    assert(!giulia_pcie_power_pipe_enable(&p));
    assert(!giulia_pcie_power_pipe_disable(&p));
    assert(!giulia_pcie_power_stop(&p, stop)); restored(&m, &p);

    /* Cancel before submitting a managed profile: no reset is pulsed. */
    init(&m); memset(&p,0,sizeof(p)); v = io(&m);
    assert(!giulia_pcie_power_prepare_host(&p,&v,flags));
    assert(!giulia_pcie_power_stop(&p,stop)); restored(&m,&p);
    for (unsigned i = 0; i < m.writes; i++)
        assert(m.events[i].off != 0x6b000 && m.events[i].off != 0x6c01c);

    /* Direct ownership still requires a ready NoC before host preparation. */
    init(&m); memset(&p,0,sizeof(p)); v = io(&m);
    assert(giulia_pcie_power_prepare_host(&p,&v,GIULIA_POWER_HOST_REQUIRED | GIULIA_POWER_DIRECT_VOTES)
        == GIULIA_POWER_PREREQUISITE && !m.reads);
    assert(!giulia_pcie_power_prepare_host(&p,&v,GIULIA_POWER_REQUIRED));
    assert(!giulia_pcie_power_finish_reset(&p,GIULIA_POWER_REQUIRED));
    assert(!giulia_pcie_power_stop(&p,GIULIA_POWER_STOP_REQUIRED)); restored(&m,&p);

    /* Before the resource phase, ordinary prepare failures can still unwind. */
    init(&m); memset(&p,0,sizeof(p)); v = io(&m); m.gdsc_stuck = BIT(0);
    assert(giulia_pcie_power_prepare_host(&p,&v,flags) == GIULIA_POWER_TIMEOUT);
    restored(&m,&p);

    /* After that phase, reset failures must keep its GCC foundation alive. */
    init(&m); memset(&p,0,sizeof(p)); v = io(&m);
    assert(!giulia_pcie_power_prepare_host(&p,&v,flags));
    m.blocked = 1; m.block_off = 0x6b000; m.block_mask = 1;
    assert(giulia_pcie_power_finish_reset(&p,finish) == GIULIA_POWER_READBACK);
    assert(p.state == GIULIA_POWER_FAULT && p.dirty && !p.cleanup_error);
    assert(p.reset_attempted);
    assert((raw(&m,0,0x52008) & 0x7f) == 0x6f && !(raw(&m,0,0x5214c) & 9));
    before = m.writes;
    assert(giulia_pcie_power_stop(&p,GIULIA_POWER_STOP_REQUIRED) == GIULIA_POWER_PREREQUISITE);
    assert(m.writes == before);
    m.blocked = 0; assert(!giulia_pcie_power_stop(&p,stop)); restored(&m,&p);

    for (unsigned nth = 1; nth <= finish_writes; nth++) {
        init(&m); memset(&p,0,sizeof(p)); v = io(&m);
        assert(!giulia_pcie_power_prepare_host(&p,&v,flags));
        m.drop_at = m.writes + nth;
        int rc = giulia_pcie_power_finish_reset(&p,finish);
        assert(rc == 0 || rc == GIULIA_POWER_READBACK || rc == GIULIA_POWER_TIMEOUT);
        if (rc) assert(p.state == GIULIA_POWER_FAULT && p.dirty);
        m.drop_at = 0;
        assert(!giulia_pcie_power_stop(&p,stop)); restored(&m,&p);
    }
    puts("SM8650 PCIe staged power: host clocks precede managed resources/reset; failure retains foundation");
}

int main(void)
{
    lifecycle();
    admission();
    inherited_rchng();
    timeouts();
    readback_and_retry();
    inaccessible_mmio();
    staged_cesta();
    puts("SM8650 PCIe0 power/clock/reset tests passed");
    return 0;
}
