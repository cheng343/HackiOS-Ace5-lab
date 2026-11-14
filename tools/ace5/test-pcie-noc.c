/* SPDX-License-Identifier: BSD-2-Clause */
/* Cloud-only fixture linked to the real read-only giulia_pcie_noc.c. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "giulia_pcie_noc.h"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
struct reg { unsigned block; uint32_t off, value; unsigned visits; };
struct mock {
    struct reg regs[300];
    unsigned count, reads, fail_at;
    unsigned change_block;
    uint32_t change_off;
    unsigned change_enabled;
};

static struct reg *find(struct mock *m, unsigned block, uint32_t off)
{
    unsigned i;
    for (i = 0; i < m->count; i++)
        if (m->regs[i].block == block && m->regs[i].off == off)
            return &m->regs[i];
    assert(!"read outside independently enumerated register set");
    return NULL;
}
static void seed(struct mock *m, unsigned block, uint32_t off, uint32_t value)
{
    unsigned i;
    struct reg *r;
    for (i = 0; i < m->count; i++)
        assert(m->regs[i].block != block || m->regs[i].off != off);
    assert(m->count < COUNT(m->regs));
    r = &m->regs[m->count++];
    r->block = block; r->off = off; r->value = value;
}
static uint32_t tag(unsigned block, uint32_t off)
{ return 0xa0000000u | (block << 24) | off; }

static void init(struct mock *m, unsigned perf, unsigned bw)
{
    static const uint32_t sm[] = {
        0x1040, 0x1048, 0x1050, 0x1058, 0x1060, 0x1068,
        0x1070, 0x1078, 0x1080, 0x1088, 0x1090, 0x109c
    };
    /* Absolute HW0 state starts, independently transcribed from crm.c. */
    static const uint32_t perf_start[] = { 0x200, 0x240, 0x280, 0x800, 0x840 };
    static const uint32_t bw_start[] = { 0x2c0, 0x340, 0x880, 0x900, 0x980 };
    unsigned i, state, word;
    memset(m, 0, sizeof(*m));
    for (i = 0; i < COUNT(sm); i++) seed(m, 0, sm[i], tag(0, sm[i]));
    seed(m, 1, 0, 0x00010200u); /* fixture only, not observed silicon version */
    seed(m, 1, 4, (3u << 24) | (1u << 20) | (2u << 16) |
        (2u << 12) | (perf << 8) | 0x25u);
    seed(m, 1, 8, bw << 26);
    seed(m, 1, 12, 1);
    seed(m, 1, 0x1000, 2);
    seed(m, 1, 0x1020, 2);
    for (state = 0; state < 5; state++) {
        for (word = 0; word < perf * 2; word++)
            seed(m, 1, perf_start[state] + word * 4,
                tag(1, perf_start[state] + word * 4));
        for (word = 0; word < bw * 2; word++)
            seed(m, 1, bw_start[state] + word * 4,
                tag(1, bw_start[state] + word * 4));
    }
    for (i = 0; i < perf; i++) seed(m, 1, 0x3c0 + i * 4, 4u + i);
    seed(m, 1, 0x3f0, 0x42);
}

static int read32(void *cookie, unsigned block, uint32_t off, uint32_t *value)
{
    struct mock *m = cookie;
    struct reg *r = find(m, block, off);
    assert(++m->reads <= 285); /* worst supported dimensions, no polling */
    assert(off != 0x1094 && off != 0x1098);
    if (m->reads == m->fail_at) return -77;
    *value = r->value;
    if (r->visits++ && m->change_enabled && block == m->change_block &&
        off == m->change_off) *value ^= 1;
    return 0;
}
static struct giulia_pcie_noc_io io_for(struct mock *m)
{
    struct giulia_pcie_noc_io io = {
        m, read32, GIULIA_NOC_SM_PHYS, GIULIA_NOC_SM_SIZE,
        GIULIA_NOC_CRM_PHYS, GIULIA_NOC_CRM_SIZE
    };
    return io;
}

int main(void)
{
    struct mock m;
    struct giulia_pcie_noc_io io;
    struct giulia_pcie_noc_snapshot s;
    unsigned i, successful_reads;
    int rc;
    init(&m, 2, 6); io = io_for(&m);
    assert(giulia_pcie_noc_snapshot(NULL, 3, &s) == GIULIA_NOC_ARGUMENT);
    assert(giulia_pcie_noc_snapshot(&io, 3, NULL) == GIULIA_NOC_ARGUMENT);
    for (i = 0; i < 3; i++) {
        assert(giulia_pcie_noc_snapshot(&io, i, &s) == GIULIA_NOC_PREREQUISITE);
        assert(!m.reads && !s.valid);
    }
    io.sm_phys += 0x3000;
    assert(giulia_pcie_noc_snapshot(&io, 3, &s) == GIULIA_NOC_LAYOUT);
    io = io_for(&m); io.crm_size -= 4;
    assert(giulia_pcie_noc_snapshot(&io, 3, &s) == GIULIA_NOC_LAYOUT);
    io = io_for(&m); io.crm_phys += 0x800;
    assert(giulia_pcie_noc_snapshot(&io, 3, &s) == GIULIA_NOC_LAYOUT);
    io = io_for(&m); io.sm_size += 4;
    assert(giulia_pcie_noc_snapshot(&io, 3, &s) == GIULIA_NOC_LAYOUT);
    assert(!m.reads);

    io = io_for(&m);
    assert(giulia_pcie_noc_snapshot(&io, 3, &s) == GIULIA_NOC_OK);
    assert(s.valid == 0x3f && s.read_count == 119 && m.reads == 119);
    assert(s.major == 1 && s.minor == 2 && s.hw_drvs == 2 && s.sw_drvs == 1);
    assert(s.perf_resources == 2 && s.bw_resources == 6 && s.bw_vcds == 3);
    assert(s.channel[GIULIA_NOC_CH_UPDATE] == 2);
    assert(s.sm[GIULIA_NOC_SM_START] == tag(0, 0x1090));
    assert(s.sm[GIULIA_NOC_SM_CLKREQ_STATUS] == tag(0, 0x109c));
    assert(s.perf[0][0][0] == tag(1, 0x200));
    assert(s.perf[1][0][0] == tag(1, 0x208));
    assert(s.perf[1][4][1] == tag(1, 0x84c));
    assert(s.bw[1][0][0] == tag(1, 0x2d8));
    assert(s.bw[1][4][5] == tag(1, 0x9ac));
    assert(s.perf_status[1] == 5 && s.pwr_idx_status == 0x42);
    assert(!s.perf[0][0][2] && !s.bw[0][0][6]);
    successful_reads = m.reads;

    /* Any callback failure is propagated immediately with partial-section
     * validity; no retry, channel change, or fallback programming exists. */
    for (i = 1; i <= successful_reads; i++) {
        init(&m, 2, 6); io = io_for(&m); m.fail_at = i;
        assert(giulia_pcie_noc_snapshot(&io, 3, &s) == GIULIA_NOC_READ_ERROR);
        assert(m.reads == i && s.read_count == i && s.callback_error == -77);
        assert(!(s.valid & GIULIA_NOC_HAVE_AFTER));
        assert(find(&m, s.failed_block, s.failed_offset));
    }
    init(&m, 2, 6); io = io_for(&m); find(&m, 1, 12)->value = 0;
    assert(giulia_pcie_noc_snapshot(&io, 3, &s) == GIULIA_NOC_DISABLED);
    assert(m.reads == 16 && s.valid == (GIULIA_NOC_HAVE_SM | GIULIA_NOC_HAVE_HEADER));

    /* Unsupported dimensions and open-bus signatures stop at the header. */
    for (i = 0; i < 7; i++) {
        init(&m, 2, 6); io = io_for(&m);
        switch (i) {
        case 0: find(&m, 1, 4)->value &= ~(15u << 4); break;
        case 1: find(&m, 1, 4)->value ^= 1; break;
        case 2: find(&m, 1, 4)->value &= ~(15u << 16); break;
        case 3: find(&m, 1, 4)->value |= 15u << 8; break;
        case 4: find(&m, 1, 8)->value = 17u << 26; break;
        case 5: find(&m, 1, 8)->value = 0; break;
        default: find(&m, 1, 0)->value = 0xffffffffu; break;
        }
        assert(giulia_pcie_noc_snapshot(&io, 3, &s) == GIULIA_NOC_LAYOUT);
        assert(m.reads == 16 && !(s.valid & GIULIA_NOC_HAVE_TABLES));
    }
    for (i = 0; i < 3; i++) {
        init(&m, 2, 6); io = io_for(&m); m.change_enabled = 1;
        m.change_block = i ? 1 : 0;
        m.change_off = i == 0 ? 0x1088 : i == 1 ? 4 : 0x1020;
        assert(giulia_pcie_noc_snapshot(&io, 3, &s) == GIULIA_NOC_OK);
        assert(s.valid == 0x7f); /* still only a non-atomic observation */
    }
    init(&m, 8, 16); io = io_for(&m);
    rc = giulia_pcie_noc_snapshot(&io, 3, &s);
    assert(rc == GIULIA_NOC_OK && m.reads == 285 && s.valid == 0x3f);
    assert(s.perf[1][4][7] == tag(1, 0x87c));
    assert(s.bw[1][4][15] == tag(1, 0x9fc));
    assert(s.perf_status[7] == 11);
    puts("pcie-noc: read-only capture, resource strides, failures and change detection passed");
    return 0;
}
