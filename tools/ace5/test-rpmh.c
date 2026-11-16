/* SPDX-License-Identifier: BSD-2-Clause */
/* Cloud-only: link real giulia_rpmh.c + giulia_wlan_rails.c. Synthetic CMD DB
 * version/resource addresses below are test data, NOT observed device values.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "giulia_wlan_rails.h"
#define BIT(n) (1u << (n))
#define TCS 0xd00u
#define WAIT (TCS + 0x20u)
#define CTRL (TCS + 0x24u)
#define STATUS (TCS + 0x28u)
#define ENABLE (TCS + 0x2cu)
#define MSG (TCS + 0x34u)
#define CSTATUS (TCS + 0x40u)
struct mock {
    uint32_t regs[0x2000 / 4], original[0x2000 / 4];
    uint32_t vote[7][3];
#ifdef GIULIA_RPMH_CPU_NOC_TEST
    uint32_t cpu_vote[2];
#endif
#ifdef GIULIA_RPMH_GEM_TEST
    uint32_t arc_vote[2], arc_data[32];
#endif
    uint32_t command[32][3], count[32];
    uint32_t allones, drop, cleanup_block;
    unsigned reads, writes, barriers, elapsed, triggers, pending, hold_at;
    unsigned incomplete, changed_irq, partial_words;
};
static void set(struct mock *m, uint32_t o, uint32_t v) { m->regs[o / 4] = v; }
static uint32_t get(struct mock *m, uint32_t o) { return m->regs[o / 4]; }
static void init(struct mock *m)
{
    unsigned i, j;
    memset(m, 0, sizeof(*m));
    set(m, 0, 3u << 16); set(m, 0x0c, (8u << 12) | (16u << 27));
    set(m, TCS, BIT(3)); set(m, WAIT, 0x20);
    for (i = 0; i < 3; i++) set(m, STATUS + i * 672, 1);
    for (i = 0; i < 16; i++) for (j = 0; j < 3; j++)
        set(m, MSG + i * 24 + j * 4, 0x8000u + i * 4 + j);
    memcpy(m->original, m->regs, sizeof(m->regs));
    for (i = 0; i < 7; i++) {
        m->vote[i][0] = 100 + i; m->vote[i][1] = i & 1u;
        m->vote[i][2] = i < 2 ? 4 : (i == 6 ? 7 : 99);
    }
    m->partial_words = 1;
}
static void execute(struct mock *m, int partial)
{
    uint32_t mask = get(m, ENABLE);
    unsigned i;
    assert((get(m, WAIT) & mask) == mask);
    for (i = 0; i < 16; i++) {
        uint32_t addr, value;
        if (!(mask & BIT(i))) continue;
        assert(get(m, MSG + i * 24) == 0x10108);
        addr = get(m, MSG + i * 24 + 4); value = get(m, MSG + i * 24 + 8);
#ifdef GIULIA_RPMH_GEM_TEST
        if (addr == 0x31000 || addr == 0x31010) {
            assert(value < 16 && i == 0);
            m->arc_vote[(addr - 0x31000) / 16] = value;
            m->arc_data[m->triggers-1] = value;
        } else
#endif
#ifdef GIULIA_RPMH_CPU_NOC_TEST
        if (addr == 0x51000 || addr == 0x51004) {
            assert(value == 0x60000001u); /* Binary ON only; never zero/disable. */
            m->cpu_vote[(addr - 0x51000) / 4] = value;
        } else
#endif
        {
            assert(addr >= 0x40000 && addr <= 0x40068 && !(addr & 3u));
            assert((addr & 15u) <= 8);
            m->vote[(addr - 0x40000) / 16][(addr & 15u) / 4] = value;
        }
        set(m, CSTATUS + i * 24, BIT(8) | (m->incomplete ? 0 : BIT(16)));
        if (partial && i + 1 == m->partial_words) return;
    }
    set(m, STATUS, 1); set(m, TCS + 4, get(m, TCS + 4) | 1u);
    m->pending = 0;
}
static uint32_t read32(void *cookie, uint32_t off)
{
    struct mock *m = cookie;
    assert(off < 0x2000 && !(off & 3));
    assert(++m->reads < 100000);
    if (off && off == m->allones) return 0xffffffffu;
    if (m->pending && (!m->hold_at || m->triggers != m->hold_at)) execute(m, 0);
    return get(m, off);
}
static void write32(void *cookie, uint32_t off, uint32_t value)
{
    struct mock *m = cookie;
    assert(off >= TCS && off < TCS + 672 && !(off & 3));
    m->writes++;
    if (off == m->drop) return;
    if (m->cleanup_block && m->triggers && off == WAIT && value == m->original[WAIT / 4]) return;
    if (off == TCS + 8) { set(m, TCS + 4, get(m, TCS + 4) & ~value); return; }
    set(m, off, value);
    if (off == CTRL && (value & BIT(24))) {
        unsigned n = 0;
        assert(m->triggers < 32);
        for (unsigned i = 0; i < 16; i++) if (get(m,ENABLE) & BIT(i)) {
            assert(n < 3);
            m->command[m->triggers][n++] = get(m,MSG + i*24 + 4);
        }
        m->count[m->triggers] = n;
        m->triggers++;
        if (m->changed_irq) set(m, TCS, get(m, TCS) | BIT(7));
        set(m, STATUS, 0); m->pending = 1;
        if (m->triggers == m->hold_at) execute(m, 1);
        else execute(m, 0);
    }
}
static void barrier(void *cookie) { ((struct mock *)cookie)->barriers++; }
static void delay_us(void *cookie, unsigned us)
{
    struct mock *m = cookie;
    assert(us && us <= 100); m->elapsed += us; assert(m->elapsed <= 1000000);
}
static void setup(struct mock *m, struct giulia_rpmh *p)
{
    struct giulia_rpmh_io io = {m, read32, write32, barrier, delay_us};
    init(m); memset(p, 0, sizeof(*p));
    assert(giulia_rpmh_init(p, &io, GIULIA_RPMH_REQUIRED) == 0);
    assert(m->reads == 0 && m->writes == 0);
}
static void restored(struct mock *m, struct giulia_rpmh *p)
{
    unsigned i, j;
    assert(p->state == GIULIA_RPMH_DONE && !p->dirty && !giulia_rpmh_bus_owned(p));
    assert(get(m, CTRL) == m->original[CTRL / 4]);
    assert(get(m, WAIT) == m->original[WAIT / 4] && get(m, ENABLE) == 0);
    assert(get(m, TCS) == (BIT(3) | (m->changed_irq ? BIT(7) : 0)));
    assert(!(get(m, TCS + 4) & 1));
    for (i = 0; i < 16; i++) for (j = 0; j < 3; j++)
        assert(get(m, MSG + i * 24 + j * 4) == m->original[(MSG + i * 24 + j * 4) / 4]);
    assert(m->writes == m->barriers);
}
static void transport_tests(void)
{
    struct mock m;
    struct giulia_rpmh p;
    const struct giulia_rpmh_command cmd[2] = {{0x40000, 1800}, {0x40004, 1}};
    int peer, wrong;
    unsigned count;
    setup(&m, &p); m.changed_irq = 1;
    assert(giulia_rpmh_init(&p, &p.io, GIULIA_RPMH_REQUIRED) == 0);
    assert(giulia_rpmh_bus_claim(&peer) == 0);
    assert(giulia_rpmh_bus_claim(&peer) == GIULIA_RPMH_BUSY);
    assert(giulia_rpmh_write(&p, cmd, 2) == GIULIA_RPMH_BUSY && m.reads == 0);
    assert(giulia_rpmh_bus_release(&wrong) == GIULIA_RPMH_STATE);
    assert(giulia_rpmh_bus_release(&peer) == 0);
    assert(giulia_rpmh_write(&p, cmd, 2) == 0);
    assert(p.completed && m.triggers == 1 && m.vote[0][0] == 1800 && m.vote[0][1] == 1);
    restored(&m, &p); /* Restored TCS buffers did NOT restore remote votes. */

    setup(&m, &p); set(&m, ENABLE + 672, 1);
    assert(giulia_rpmh_write(&p, cmd, 2) == GIULIA_RPMH_BUSY && m.writes == 0);
    assert(!giulia_rpmh_bus_owned(&p));
    setup(&m, &p); set(&m, TCS + 4, 2);
    assert(giulia_rpmh_write(&p, cmd, 2) == GIULIA_RPMH_BUSY && m.writes == 0);
    setup(&m, &p); set(&m, 0, 2u << 16);
    assert(giulia_rpmh_write(&p, cmd, 2) == GIULIA_RPMH_PREREQUISITE && m.writes == 0);
    setup(&m, &p); set(&m, 0x0c, (8u << 12) | (1u << 27));
    assert(giulia_rpmh_write(&p, cmd, 2) == GIULIA_RPMH_PREREQUISITE && m.writes == 0);

    setup(&m, &p); m.hold_at = 1;
    assert(giulia_rpmh_write(&p, cmd, 2) == GIULIA_RPMH_TIMEOUT);
    assert(p.state == GIULIA_RPMH_FAULT && p.triggered && !p.completed);
    assert(giulia_rpmh_bus_owned(&p) && giulia_rpmh_bus_claim(&peer) == GIULIA_RPMH_BUSY);
    count = m.writes;
    assert(giulia_rpmh_recover(&p) == GIULIA_RPMH_TIMEOUT && m.writes == count);
    m.hold_at = 0;
    assert(giulia_rpmh_recover(&p) == 0 && m.triggers == 1);
    restored(&m, &p);

    setup(&m, &p); m.cleanup_block = 1;
    assert(giulia_rpmh_write(&p, cmd, 2) == GIULIA_RPMH_READBACK);
    assert(p.completed && giulia_rpmh_bus_owned(&p));
    m.cleanup_block = 0;
    assert(giulia_rpmh_recover(&p) == 0 && m.triggers == 1);
    restored(&m, &p);

    setup(&m, &p); m.drop = MSG + 4;
    assert(giulia_rpmh_write(&p, cmd, 2) == GIULIA_RPMH_READBACK);
    assert(!p.triggered && !p.completed && m.triggers == 0);
    restored(&m, &p);
    setup(&m, &p); m.allones = WAIT;
    assert(giulia_rpmh_write(&p, cmd, 2) == GIULIA_RPMH_READBACK && m.writes == 0);
    setup(&m, &p); m.incomplete = 1;
    assert(giulia_rpmh_write(&p, cmd, 2) == GIULIA_RPMH_TIMEOUT);
    set(&m, CSTATUS, BIT(8) | BIT(16)); set(&m, CSTATUS + 24, BIT(8) | BIT(16));
    assert(giulia_rpmh_recover(&p) == 0); restored(&m, &p);
}

static void le16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void le32(uint8_t *p, uint32_t v)
{ le16(p, v); le16(p + 2, v >> 16); }
static void database(uint8_t db[1024])
{
    const char *names[7] = {"ldob15", "ldoc3", "smpi4", "smpc2", "smpc3", "smpc6", "smpc1"};
    unsigned i;
    memset(db, 0, 1024); le32(db, 1); le32(db + 4, 0x0c0330db);
    le16(db + 8, 4); le16(db + 12, 192); le16(db + 14, 7);
    for (i = 0; i < 7; i++) {
        memcpy(db + 144 + i * 24, names[i], strlen(names[i]));
        le32(db + 160 + i * 24, 0x40000 + i * 16);
    }
}
static void db_tests(void)
{
    uint8_t db[1024]; uint32_t addr;
    database(db);
    assert(giulia_rpmh_resolve_vrm(db, sizeof(db), 1, "ldob15", &addr) == 0 && addr == 0x40000);
    assert(giulia_rpmh_resolve_vrm(db, sizeof(db), 1, "missing", &addr) == GIULIA_RPMH_NOT_FOUND);
    assert(giulia_rpmh_resolve_vrm(db, sizeof(db), 0, "ldob15", &addr) == GIULIA_RPMH_ARGUMENT);
    assert(giulia_rpmh_resolve_vrm(db, sizeof(db), 2, "ldob15", &addr) == GIULIA_RPMH_DATABASE);
    assert(giulia_rpmh_resolve_vrm(db, 143, 1, "ldob15", &addr) == GIULIA_RPMH_DATABASE);
    db[4] ^= 1;
    assert(giulia_rpmh_resolve_vrm(db, sizeof(db), 1, "ldob15", &addr) == GIULIA_RPMH_DATABASE);
    database(db); le16(db + 10, 1020);
    assert(giulia_rpmh_resolve_vrm(db, sizeof(db), 1, "ldob15", &addr) == GIULIA_RPMH_DATABASE);
    database(db); le16(db + 14, 513);
    assert(giulia_rpmh_resolve_vrm(db, sizeof(db), 1, "ldob15", &addr) == GIULIA_RPMH_DATABASE);
    database(db); le16(db + 164, 1024);
    assert(giulia_rpmh_resolve_vrm(db, sizeof(db), 1, "ldob15", &addr) == GIULIA_RPMH_DATABASE);
    database(db); le16(db + 8, 3);
    assert(giulia_rpmh_resolve_vrm(db, sizeof(db), 1, "ldob15", &addr) == GIULIA_RPMH_DATABASE);
    database(db); le32(db + 160, 0x50000);
    assert(giulia_rpmh_resolve_vrm(db, sizeof(db), 1, "ldob15", &addr) == GIULIA_RPMH_DATABASE);
    database(db); le32(db + 160, 0x40004);
    assert(giulia_rpmh_resolve_vrm(db, sizeof(db), 1, "ldob15", &addr) == GIULIA_RPMH_DATABASE);
    database(db); memcpy(db + 168, db + 144, 24);
    assert(giulia_rpmh_resolve_vrm(db, sizeof(db), 1, "ldob15", &addr) == GIULIA_RPMH_DATABASE);
    database(db); memcpy(db + 24, db + 8, 16); memset(db + 8, 0, 16);
    assert(giulia_rpmh_resolve_vrm(db, sizeof(db), 1, "ldob15", &addr) == GIULIA_RPMH_DATABASE);
}
static void baselines(struct giulia_wlan_rail_baseline b[7])
{
    unsigned i;
    for (i = 0; i < 7; i++) {
        b[i].millivolts = 100 + i; b[i].enable = i & 1u; b[i].known = 3;
        b[i].mode = i < 2 ? 4 : (i == 6 ? 7 : 99);
    }
}
static void votes_restored(struct mock *m)
{
    unsigned i;
    for (i = 0; i < 7; i++) {
        assert(m->vote[i][0] == 100 + i && m->vote[i][1] == (i & 1u));
        assert(m->vote[i][2] == (i < 2 ? 4u : (i == 6 ? 7u : 99u)));
    }
}
static void rail_tests(void)
{
    struct mock m;
    struct giulia_rpmh t;
    struct giulia_wlan_rails p = {0}, other = {0};
    struct giulia_wlan_rail_baseline b[7];
    uint8_t db[1024];
    const uint32_t expected[7] = {1800,1200,940,976,916,1864,1316};
    unsigned i, count;
    database(db); baselines(b); setup(&m, &t);
    assert(giulia_wlan_rails_start(&p,&t,db,sizeof(db),1,NULL,
        GIULIA_WLAN_RAIL_REQUIRED,GIULIA_WLAN_RESTORE_BASELINE) == GIULIA_WLAN_RAIL_BASELINE_UNKNOWN);
    assert(m.reads == 0);
    b[2].known = 1;
    assert(giulia_wlan_rails_start(&p,&t,db,sizeof(db),1,b,
        GIULIA_WLAN_RAIL_REQUIRED,GIULIA_WLAN_RESTORE_BASELINE) == GIULIA_WLAN_RAIL_BASELINE_UNKNOWN);
    b[2].known = 3;
    le32(db + 160 + 24, 0x40000); /* Two names must not alias one resource. */
    assert(giulia_wlan_rails_start(&p,&t,db,sizeof(db),1,b,
        GIULIA_WLAN_RAIL_REQUIRED,GIULIA_WLAN_RESTORE_BASELINE) == GIULIA_RPMH_DATABASE);
    assert(m.reads == 0); database(db);
    assert(giulia_wlan_rails_start(&p,&t,db,sizeof(db),1,b,
        GIULIA_WLAN_RAIL_REQUIRED,GIULIA_WLAN_RESTORE_BASELINE) == 0);
    assert(p.state == GIULIA_WLAN_RAIL_ACTIVE && p.completed_mask == 127 && m.triggers == 7);
    for (i = 0; i < 7; i++) assert(m.vote[i][0] == expected[i] && m.vote[i][1] == 1);
    count = m.writes;
    assert(giulia_wlan_rails_stop(&p,0) == GIULIA_RPMH_PREREQUISITE && m.writes == count);
    assert(giulia_wlan_rails_start(&other,&t,db,sizeof(db),1,b,
        GIULIA_WLAN_RAIL_REQUIRED,GIULIA_WLAN_RESTORE_BASELINE) == GIULIA_RPMH_BUSY);
    assert(giulia_wlan_rails_stop(&p,1) == 0 && m.triggers == 14);
    votes_restored(&m); restored(&m, &t);

    memset(&p,0,sizeof(p)); setup(&m,&t); m.hold_at = 3;
    assert(giulia_wlan_rails_start(&p,&t,db,sizeof(db),1,b,
        GIULIA_WLAN_RAIL_REQUIRED,GIULIA_WLAN_RESTORE_BASELINE) == GIULIA_RPMH_TIMEOUT);
    assert(p.state == GIULIA_WLAN_RAIL_FAULT && p.attempted_mask == 7);
    assert(giulia_rpmh_bus_owned(&t));
    m.hold_at = 0;
    assert(giulia_wlan_rails_stop(&p,1) == 0 && m.triggers == 6);
    votes_restored(&m); restored(&m, &t);
}
static void forward_only_last(void)
{
    static struct mock m;
    static struct giulia_rpmh t;
    static struct giulia_wlan_rails p;
    uint8_t db[1024]; unsigned count;
    database(db); setup(&m,&t);
    assert(giulia_wlan_rails_start(&p,&t,db,sizeof(db),1,NULL,
        GIULIA_WLAN_RAIL_REQUIRED,GIULIA_WLAN_KEEP_VOTES) == 0);
    count = m.writes;
    assert(giulia_wlan_rails_stop(&p,1) == GIULIA_WLAN_RAIL_RETAINED);
    assert(p.state == GIULIA_WLAN_RAIL_HELD && m.writes == count);
    assert(m.vote[0][0] == 1800 && m.vote[0][1] == 1);
    assert(giulia_wlan_rails_stop(&p,1) == GIULIA_WLAN_RAIL_RETAINED);
}
static void mode_tests(void)
{
    struct mock m; struct giulia_rpmh t; struct giulia_wlan_rails p = {0};
    struct giulia_wlan_rail_baseline b[7]; uint8_t db[1024];
    unsigned flags = GIULIA_WLAN_RAIL_REQUIRED | GIULIA_WLAN_RAIL_MODE_WRITE_SAFE;
    database(db); baselines(b); setup(&m,&t);
    assert(giulia_wlan_rails_start_with_dt_modes(&p,&t,db,sizeof(db),1,b,
        GIULIA_WLAN_RAIL_REQUIRED,0) == GIULIA_RPMH_PREREQUISITE);
    assert(giulia_wlan_rails_start_with_dt_modes(&p,&t,db,sizeof(db),1,b,flags,0)
        == GIULIA_WLAN_RAIL_BASELINE_UNKNOWN);
    b[0].known = b[1].known = b[6].known = 7; b[6].mode = 4;
    assert(giulia_wlan_rails_start_with_dt_modes(&p,&t,db,sizeof(db),1,b,flags,0)
        == GIULIA_WLAN_RAIL_BASELINE_UNKNOWN);
    assert(!m.reads && !m.writes); b[6].mode = 7;
    assert(!giulia_wlan_rails_check(db,sizeof(db),1,b,flags,GIULIA_WLAN_RESTORE_BASELINE,1));
    assert(giulia_wlan_rails_check(db,sizeof(db),1,b,flags,GIULIA_WLAN_RESTORE_BASELINE,2)==GIULIA_RPMH_ARGUMENT);
    assert(!m.reads && !m.writes);
    assert(giulia_wlan_rails_start_with_dt_modes(&p,&t,db,sizeof(db),1,b,flags,0) == 0);
    for (unsigned i = 0; i < 7; i++) {
        uint32_t base = 0x40000 + 16*i;
        unsigned managed = i < 2 || i == 6;
        assert(m.count[i] == (managed ? 3u : 2u));
        assert(m.command[i][0] == base);
        assert(m.command[i][managed ? 2 : 1] == base+4);
        if (managed) {
            assert(m.command[i][1] == base+8);
            assert(m.vote[i][2] == 7u); /* Preserve inherited S1C HPM, DT RET is only a lower request. */
        } else assert(m.vote[i][2] == 99);
    }
    assert(giulia_wlan_rails_stop(&p,1) == 0);
    for (unsigned n = 7; n < 14; n++) {
        unsigned i = 13-n, managed = i < 2 || i == 6;
        uint32_t base = 0x40000 + 16*i;
        if (i & 1u) {
            assert(m.command[n][0] == base);
            assert(m.command[n][managed ? 2 : 1] == base+4);
            if (managed) assert(m.command[n][1] == base+8);
        } else {
            assert(m.command[n][0] == base+4 && m.command[n][1] == base);
            if (managed) assert(m.command[n][2] == base+8);
        }
    }
    votes_restored(&m); restored(&m,&t);
    memset(&p,0,sizeof(p)); setup(&m,&t); m.hold_at = 2; m.partial_words = 2;
    assert(giulia_wlan_rails_start_with_dt_modes(&p,&t,db,sizeof(db),1,b,flags,0)
        == GIULIA_RPMH_TIMEOUT);
    assert(p.attempted_mask == 3 && p.state == GIULIA_WLAN_RAIL_FAULT);
    assert(m.vote[1][2] == 7 && giulia_rpmh_bus_owned(&t));
    m.hold_at = 0;
    assert(giulia_wlan_rails_stop(&p,1) == 0 && m.triggers == 4);
    votes_restored(&m); restored(&m,&t);
    puts("Ace5 WLAN rails: DT modes, V-M-E ordering and partial mode compensation passed");
}
static void shared_votes(void)
{
    static const uint32_t wanted[7] = {1800,1200,940,976,916,1864,1316};
    struct mock m; struct giulia_rpmh t; struct giulia_wlan_rails p;
    struct giulia_wlan_rail_baseline b[7]; uint8_t db[1024];
    unsigned flags = GIULIA_WLAN_RAIL_REQUIRED | GIULIA_WLAN_RAIL_MODE_WRITE_SAFE;
    database(db);
    for (unsigned i = 0; i < 7; i++) {
        for (unsigned enabled = 0; enabled < 2; enabled++) {
            baselines(b); setup(&m,&t); memset(&p,0,sizeof(p));
            b[i].millivolts = wanted[i] + 1; b[i].enable = enabled;
            assert(giulia_wlan_rails_start(&p,&t,db,sizeof(db),1,b,
                GIULIA_WLAN_RAIL_REQUIRED,GIULIA_WLAN_RESTORE_BASELINE) == GIULIA_WLAN_RAIL_VOLTAGE_CONFLICT);
            assert(!m.reads && !m.writes && !m.triggers && p.state == GIULIA_WLAN_RAIL_IDLE);
        }
    }
    const uint32_t ldo_modes[] = {3,4,7}, smps_modes[] = {3,7};
    for (unsigned ldo = 0; ldo < 3; ldo++) {
        for (unsigned smps = 0; smps < 2; smps++) {
            baselines(b); setup(&m,&t); memset(&p,0,sizeof(p));
            for (unsigned i = 0; i < 7; i++) {
                b[i].millivolts = wanted[i]; m.vote[i][0] = wanted[i];
            }
            b[0].known = b[1].known = b[6].known = 7;
            b[0].mode = b[1].mode = ldo_modes[ldo]; b[6].mode = smps_modes[smps];
            m.vote[0][2] = m.vote[1][2] = ldo_modes[ldo]; m.vote[6][2] = smps_modes[smps];
            assert(!giulia_wlan_rails_start_with_dt_modes(&p,&t,db,sizeof(db),1,b,flags,GIULIA_WLAN_RESTORE_BASELINE));
            assert(m.vote[0][2] == 7 && m.vote[1][2] == 7);
            assert(m.vote[6][2] == smps_modes[smps] && p.selected_mode[6] == smps_modes[smps]);
            for (unsigned i = 0; i < 7; i++) assert(m.vote[i][0] == wanted[i]);
            assert(!giulia_wlan_rails_stop(&p,1));
            for (unsigned i = 0; i < 7; i++) assert(m.vote[i][0] == wanted[i] && m.vote[i][1] == (i & 1u));
            assert(m.vote[0][2] == ldo_modes[ldo] && m.vote[1][2] == ldo_modes[ldo] && m.vote[6][2] == smps_modes[smps]);
            restored(&m,&t);
        }
    }
    puts("Ace5 WLAN shared votes: voltage conflicts rejected before MMIO and prior HPM preserved");
}
int main(void)
{
    transport_tests(); db_tests(); rail_tests(); mode_tests(); shared_votes(); forward_only_last();
    puts("SM8650 RPMh transport and WLAN rail vote tests passed");
    return 0;
}
