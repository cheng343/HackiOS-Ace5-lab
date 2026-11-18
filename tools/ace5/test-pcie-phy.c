/* SPDX-License-Identifier: BSD-2-Clause */
/* Actual QMP PHY lifecycle against register/clock fault injection. */
#include "giulia_pcie_phy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define RESET 0x200u
#define STATUS 0x214u
#define POWER 0x240u
#define START 0x244u
struct fixture {
    uint32_t regs[0x2000 / 4], original[0x2000 / 4];
    uint32_t ignore_write;
    unsigned reads, writes, delays, elapsed_us, status_reads;
    unsigned ready_after, pipe_on, pipe_off, pipe_live, pcs_first;
    int pipe_on_fail, pipe_off_fail, status_bad;
};
static uint32_t rd(void *cookie, uint32_t off)
{
    struct fixture *f = cookie;
    assert(off < sizeof(f->regs) && !(off & 3));
    ++f->reads;
    if (off == STATUS) {
        ++f->status_reads;
        if (f->status_bad) return UINT32_MAX;
        return f->status_reads > f->ready_after ? 0 : 0x40;
    }
    return f->regs[off / 4];
}
static void wr(void *cookie, uint32_t off, uint32_t value)
{
    struct fixture *f = cookie;
    assert(off < sizeof(f->regs) && !(off & 3));
    ++f->writes;
    if (off != f->ignore_write) f->regs[off / 4] = value;
}
static void barrier(void *cookie) { (void)cookie; }
static void delay(void *cookie, uint32_t us)
{
    struct fixture *f = cookie;
    assert(us == 1000 || us == 200);
    ++f->delays; f->elapsed_us += us;
}
static int pipe_on(void *cookie)
{
    struct fixture *f = cookie;
    ++f->pipe_on; f->pipe_live = 1;
    if (f->pcs_first) {
        assert(!(f->regs[RESET / 4] & 1) && (f->regs[START / 4] & 3) == 3);
        assert(f->elapsed_us == 1000);
    } else {
        assert((f->regs[RESET / 4] & 1) && !(f->regs[START / 4] & 3));
    }
    assert((f->regs[POWER / 4] & 3) == 3);
    /* Ace 5 TX values differ from the older reference board, on both lanes. */
    assert(f->regs[0x0e40 / 4] == 9 && f->regs[0x1640 / 4] == 9);
    assert(f->regs[0x0e3c / 4] == 0x15 && f->regs[0x163c / 4] == 0x15);
    assert(f->regs[0x0368 / 4] == 0x17 && f->regs[0x0370 / 4] == 0x2e);
    return f->pipe_on_fail;
}
static int pipe_off(void *cookie)
{
    struct fixture *f = cookie;
    ++f->pipe_off;
    if (f->pipe_off_fail) return -1;
    f->pipe_live = 0;
    return 0;
}
static struct giulia_pcie_phy_io io(struct fixture *f)
{
    return (struct giulia_pcie_phy_io){ f, rd, wr, barrier, delay, pipe_on, pipe_off };
}
static void setup(struct fixture *f, struct giulia_pcie_phy *p)
{
    memset(f, 0, sizeof(*f)); memset(p, 0, sizeof(*p));
    for (unsigned i = 0; i < sizeof(f->regs)/sizeof(f->regs[0]); ++i)
        f->regs[i] = 0xab000080;
    f->regs[RESET / 4] = 0xab000001;
    f->regs[START / 4] = f->regs[POWER / 4] = 0xab000000;
    memcpy(f->original, f->regs, sizeof(f->regs));
    f->ignore_write = UINT32_MAX;
    f->ready_after = 2;
}
static void restored(struct fixture *f, struct giulia_pcie_phy *p)
{
    assert(p->state == GIULIA_PHY_STOPPED && !p->saved_count && !p->pipe_owned);
    assert(!f->pipe_live);
    assert(!memcmp(f->regs, f->original, sizeof(f->regs)));
}
int main(void)
{
    struct fixture f;
    struct giulia_pcie_phy p;
    struct giulia_pcie_phy_io bus = io(&f);
    unsigned before;
    for (unsigned bit = 0; bit < 5; ++bit) {
        setup(&f, &p);
        assert(giulia_pcie_phy_start(&p, GIULIA_PHY_START_REQUIRED & ~(1u << bit), &bus)
            == GIULIA_PHY_PREREQUISITE);
        assert(!f.reads && !f.writes && !f.pipe_on);
    }
    setup(&f, &p); f.regs[POWER / 4] |= 3;
    assert(giulia_pcie_phy_start(&p, GIULIA_PHY_START_REQUIRED, &bus) == GIULIA_PHY_BUSY);
    assert(!f.writes && !f.pipe_on);
    setup(&f, &p); f.regs[START / 4] |= 3;
    assert(giulia_pcie_phy_start(&p, GIULIA_PHY_START_REQUIRED, &bus) == GIULIA_PHY_BUSY);
    assert(!f.writes);
    setup(&f, &p); f.regs[RESET / 4] = UINT32_MAX;
    assert(giulia_pcie_phy_start(&p, GIULIA_PHY_START_REQUIRED, &bus) == GIULIA_PHY_READBACK);
    assert(!f.writes);

    setup(&f, &p);
    assert(!giulia_pcie_phy_start(&p, GIULIA_PHY_START_REQUIRED, &bus));
    assert(p.state == GIULIA_PHY_READY && p.saved_count == 112 && f.pipe_on == 1);
    assert(p.polls == 3 && f.elapsed_us == 1400 && f.pipe_live);
    assert(!(f.regs[RESET / 4] & 1) && (f.regs[START / 4] & 3) == 3);
    before = f.writes;
    assert(giulia_pcie_phy_start(&p, GIULIA_PHY_START_REQUIRED, &bus) == GIULIA_PHY_STATE);
    assert(giulia_pcie_phy_stop(&p, GIULIA_PHY_STOP_REQUIRED & ~GIULIA_PHY_PERST_ASSERTED)
        == GIULIA_PHY_PREREQUISITE);
    assert(f.writes == before && !f.pipe_off);
    assert(!giulia_pcie_phy_stop(&p, GIULIA_PHY_STOP_REQUIRED));
    restored(&f, &p);
    before = f.reads + f.writes;
    assert(!giulia_pcie_phy_stop(&p, 0) && before == f.reads + f.writes);

    setup(&f, &p);
    f.pcs_first = 1;
    assert(!giulia_pcie_phy_start(&p, GIULIA_PHY_START_REQUIRED | GIULIA_PHY_PCS_BEFORE_PIPE, &bus));
    assert(p.state == GIULIA_PHY_READY && f.pipe_on == 1 && !giulia_pcie_phy_stop(&p, GIULIA_PHY_STOP_REQUIRED));
    restored(&f, &p);
    setup(&f, &p);
    f.ignore_write = START;
    assert(giulia_pcie_phy_start(&p, GIULIA_PHY_START_REQUIRED | GIULIA_PHY_PCS_BEFORE_PIPE, &bus)
        == GIULIA_PHY_READBACK);
    assert(!f.pipe_on && !f.pipe_off);
    restored(&f, &p);

    setup(&f, &p); f.ready_after = 1000;
    assert(giulia_pcie_phy_start(&p, GIULIA_PHY_START_REQUIRED, &bus) == GIULIA_PHY_TIMEOUT);
    assert(f.elapsed_us == 11000 && p.polls == 51 && f.pipe_off == 1);
    assert(p.failure == GIULIA_PHY_TIMEOUT); restored(&f, &p);
    setup(&f, &p); f.status_bad = 1;
    assert(giulia_pcie_phy_start(&p, GIULIA_PHY_START_REQUIRED, &bus) == GIULIA_PHY_READBACK);
    assert(p.last_status == UINT32_MAX); restored(&f, &p);

    setup(&f, &p); f.pipe_on_fail = 1;
    assert(giulia_pcie_phy_start(&p, GIULIA_PHY_START_REQUIRED, &bus) == GIULIA_PHY_PIPE);
    assert(f.pipe_on == 1 && f.pipe_off == 1 && !f.status_reads); restored(&f, &p);
    setup(&f, &p); f.pipe_on_fail = f.pipe_off_fail = 1;
    assert(giulia_pcie_phy_start(&p, GIULIA_PHY_START_REQUIRED, &bus) == GIULIA_PHY_PIPE);
    assert(p.state == GIULIA_PHY_FAULT && p.pipe_owned && p.saved_count == 112);
    f.pipe_off_fail = 0;
    assert(!giulia_pcie_phy_stop(&p, GIULIA_PHY_STOP_REQUIRED)); restored(&f, &p);

    setup(&f, &p); f.ignore_write = 0x00c0;
    assert(giulia_pcie_phy_start(&p, GIULIA_PHY_START_REQUIRED, &bus) == GIULIA_PHY_READBACK);
    assert(!f.pipe_on); restored(&f, &p);
    setup(&f, &p); f.ignore_write = START;
    assert(giulia_pcie_phy_start(&p, GIULIA_PHY_START_REQUIRED, &bus) == GIULIA_PHY_READBACK);
    assert(f.pipe_on == 1 && f.pipe_off == 1); restored(&f, &p);

    setup(&f, &p);
    assert(!giulia_pcie_phy_start(&p, GIULIA_PHY_START_REQUIRED, &bus));
    f.ignore_write = 0x00c0;
    assert(giulia_pcie_phy_stop(&p, GIULIA_PHY_STOP_REQUIRED) == GIULIA_PHY_READBACK);
    assert(p.state == GIULIA_PHY_FAULT && p.saved_count == 112 && !p.pipe_owned);
    f.ignore_write = UINT32_MAX;
    assert(!giulia_pcie_phy_stop(&p, GIULIA_PHY_STOP_REQUIRED)); restored(&f, &p);
    puts("SM8650 QMP PHY: tuning, PIPE ordering, timeout and restore tests passed");
    return 0;
}
