/* SPDX-License-Identifier: BSD-2-Clause */
/* Cloud-only fixture linked with the production giulia_mhi_snapshot.c. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "giulia_mhi_snapshot.h"

struct reg { uint32_t off, value; unsigned visits; };
struct mock {
    struct reg regs[14];
    unsigned count, reads, fail_at, change;
    uint32_t change_off, change_value, bhi_base, bhie_base;
};
static void seed(struct mock *m, uint32_t off, uint32_t value)
{
    unsigned i;
    assert(m->count < 14);
    for (i = 0; i < m->count; i++) assert(m->regs[i].off != off);
    m->regs[m->count++] = (struct reg){off, value, 0};
}
static struct reg *find(struct mock *m, uint32_t off)
{
    unsigned i;
    for (i = 0; i < m->count; i++) if (m->regs[i].off == off) return &m->regs[i];
    assert(!"unlisted MMIO: no serial/hash/context/doorbell/window reads allowed");
    return NULL;
}
static void init(struct mock *m, uint32_t bhi, uint32_t bhie)
{
    memset(m, 0, sizeof(*m));
    m->bhi_base = bhi; m->bhie_base = bhie;
    seed(m, 0x00, 0x1000);
    seed(m, 0x08, 0x01000000);
    seed(m, 0x10, 0x02020820);
    seed(m, 0x28, bhi);
    seed(m, 0x2c, bhie);
    seed(m, 0x38, 0x100);
    seed(m, 0x48, 0x101);
    seed(m, bhi, 0);
    seed(m, bhi + 4, 1);
    seed(m, bhi + 0x28, 2);
    seed(m, bhi + 0x2c, 0x80000000);
    seed(m, bhi + 0x30, 0x31);
    seed(m, bhie + 0x44, 0x80000123);
    seed(m, bhie + 0x78, 0xffffffffu); /* undefined RX power-on word */
}
static int read32(void *cookie, uint32_t off, uint32_t *value)
{
    struct mock *m = cookie;
    struct reg *r = find(m, off);
    assert(++m->reads <= 28);
    assert(!(off & 3u) && off <= 0x1ffffc);
    /* Explicitly exclude BHI_SERIALNU/SBLANTIROLL/HWID/PKHASH, not just an
     * accidental absence from the fixture's dictionary. */
    assert(off < m->bhi_base + 0x40 || off >= m->bhi_base + 0xc4);
    if (m->reads == m->fail_at) return -91;
    *value = r->value;
    if (r->visits++ && m->change && m->change_off == off) *value = m->change_value;
    return 0;
}
static struct giulia_mhi_snapshot_io io_for(struct mock *m)
{
    return (struct giulia_mhi_snapshot_io){m, read32, GIULIA_MHI_BAR_BYTES};
}
int main(void)
{
    struct mock m;
    struct giulia_mhi_snapshot_io io;
    struct giulia_mhi_snapshot s;
    unsigned i;
    static const uint32_t bad[] = {0, 0xa4, 0x101, 0x80000100u, 0xffffff00u, 0x1ffffc};
    init(&m, 0x100, 0x300); io = io_for(&m);
    assert(giulia_mhi_snapshot(NULL, 15, &s) == GIULIA_MHI_SNAPSHOT_ARGUMENT);
    assert(giulia_mhi_snapshot(&io, 15, NULL) == GIULIA_MHI_SNAPSHOT_ARGUMENT);
    for (i = 0; i < 15; i++) {
        assert(giulia_mhi_snapshot(&io, i, &s) == GIULIA_MHI_SNAPSHOT_PREREQUISITE);
        assert(!m.reads && !s.valid);
    }
    io.bar_bytes -= 4;
    assert(giulia_mhi_snapshot(&io, 15, &s) == GIULIA_MHI_SNAPSHOT_LAYOUT && !m.reads);
    io = io_for(&m);
    assert(giulia_mhi_snapshot(&io, 15, &s) == GIULIA_MHI_SNAPSHOT_OK);
    assert(s.valid == 15 && m.reads == 28 && s.read_count == 28);
    assert(s.header[GIULIA_MHI_CONTROL] == 0x100);
    assert(s.header[GIULIA_MHI_STATUS] == 0x101);
    assert(s.bhi[GIULIA_MHI_BHI_EXECENV] == 2 && s.bhi[GIULIA_MHI_BHI_ERROR] == 0x31);
    assert(s.bhie[GIULIA_MHI_BHIE_TX_STATUS] == 0x80000123);
    assert(s.bhie[GIULIA_MHI_BHIE_RX_STATUS] == 0xffffffffu);
    for (i = 1; i <= 28; i++) {
        init(&m, 0x100, 0x300); io = io_for(&m); m.fail_at = i;
        assert(giulia_mhi_snapshot(&io, 15, &s) == GIULIA_MHI_SNAPSHOT_READ_ERROR);
        assert(m.reads == i && s.callback_error == -91);
        assert(!(s.valid & GIULIA_MHI_SNAPSHOT_HAVE_AFTER));
        assert(find(&m, s.failed_offset));
    }
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        for (unsigned word = 0x28; word <= 0x2c; word += 4) {
            init(&m, 0x100, 0x300); io = io_for(&m);
            find(&m, word)->value = bad[i];
            assert(giulia_mhi_snapshot(&io, 15, &s) == GIULIA_MHI_SNAPSHOT_LAYOUT);
            assert(m.reads == 7 && s.valid == GIULIA_MHI_SNAPSHOT_HAVE_HEADER);
        }
    }
    /* A malicious BHIE base could otherwise alias TXSTATUS into BHI_SERIAL. */
    init(&m, 0x100, 0x300); io = io_for(&m); find(&m, 0x2c)->value = 0xfc;
    assert(giulia_mhi_snapshot(&io, 15, &s) == GIULIA_MHI_SNAPSHOT_LAYOUT && m.reads == 7);
    init(&m, 0x100, 0x300); io = io_for(&m); find(&m, 0x28)->value = 0x300;
    assert(giulia_mhi_snapshot(&io, 15, &s) == GIULIA_MHI_SNAPSHOT_LAYOUT && m.reads == 7);
    init(&m, 0x100, 0x300); io = io_for(&m); find(&m, 0x08)->value = 0xffffffffu;
    assert(giulia_mhi_snapshot(&io, 15, &s) == GIULIA_MHI_SNAPSHOT_UNAVAILABLE && m.reads == 7);
    for (i = 0; i < 3; i++) {
        init(&m, 0x100, 0x300); io = io_for(&m); m.change = 1;
        m.change_off = i == 0 ? 0x48 : i == 1 ? 0x128 : 0x344;
        m.change_value = 0;
        assert(giulia_mhi_snapshot(&io, 15, &s) == GIULIA_MHI_SNAPSHOT_OK);
        assert(s.valid == 31 && m.reads == 28);
    }
    init(&m, 0x100, 0x300); io = io_for(&m);
    m.change = 1; m.change_off = 0x28; m.change_value = 0x500;
    assert(giulia_mhi_snapshot(&io, 15, &s) == GIULIA_MHI_SNAPSHOT_LAYOUT);
    assert(m.reads == 21 && s.valid == 23); /* never follow moved offsets */
    /* Last legal full footprints and a one-word overrun. */
    init(&m, 0x200000 - 0xc8, 0x300); io = io_for(&m);
    assert(giulia_mhi_snapshot(&io, 15, &s) == GIULIA_MHI_SNAPSHOT_OK && m.reads == 28);
    init(&m, 0x100, 0x200000 - 0x7c); io = io_for(&m);
    assert(giulia_mhi_snapshot(&io, 15, &s) == GIULIA_MHI_SNAPSHOT_OK && m.reads == 28);
    find(&m, 0x2c)->value += 4; m.reads = 0;
    assert(giulia_mhi_snapshot(&io, 15, &s) == GIULIA_MHI_SNAPSHOT_LAYOUT && m.reads == 7);
    puts("mhi-snapshot: read-only bounds, privacy whitelist, failure and transition tests passed");
    return 0;
}
