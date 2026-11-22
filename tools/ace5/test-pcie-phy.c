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
    unsigned status_override_at, read_at_us[102], flap_status, reset_writes_after_poll;
    uint32_t status_override;
};
static uint32_t rd(void *cookie, uint32_t off)
{
    struct fixture *f = cookie;
    assert(off < sizeof(f->regs) && !(off & 3));
    ++f->reads;
    if (off == STATUS) {
        assert(f->status_reads < sizeof(f->read_at_us) / sizeof(f->read_at_us[0]));
        f->read_at_us[f->status_reads++] = f->elapsed_us;
        if (f->status_reads == f->status_override_at) return f->status_override;
        if (f->status_bad) return UINT32_MAX;
        if (f->flap_status) return (f->status_reads & 1u) ? 0u : 0x40u;
        return f->status_reads > f->ready_after ? 0 : 0x40;
    }
    return f->regs[off / 4];
}
static void wr(void *cookie, uint32_t off, uint32_t value)
{
    struct fixture *f = cookie;
    assert(off < sizeof(f->regs) && !(off & 3));
    ++f->writes;
    if (off == RESET && f->status_reads) ++f->reset_writes_after_poll;
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
/* A clear PHYSTATUS is a candidate, not a successful PCS-first start,
 * until a second read also has bit 6 clear. Inject at the actual hardware
 * access, not in the caller or in the recorded outcome. */
static void test_stock_ready_confirmation(void)
{
    struct fixture f;
    struct giulia_pcie_phy p;
    struct giulia_pcie_phy_io bus = io(&f);
    const uint32_t stock = GIULIA_PHY_START_REQUIRED | GIULIA_PHY_PCS_BEFORE_PIPE;

    for (unsigned k = 0; k < 2; ++k) {
        const unsigned busy = k ? 50u : 0u;
        setup(&f, &p);
        f.pcs_first = 1; f.ready_after = busy;
        f.status_override_at = busy + 2u;
        f.status_override = 4u; /* bit 6 clear despite unrelated status bits */
        assert(!giulia_pcie_phy_start(&p, stock, &bus));
        assert(p.state == GIULIA_PHY_READY && f.status_reads == busy + 2u);
        assert(f.elapsed_us == 1000u + 200u * busy);
        for (unsigned j = 0; j <= busy; ++j)
            assert(f.read_at_us[j] == 1000u + 200u * j);
        assert(f.read_at_us[busy + 1u] == f.read_at_us[busy]);
        assert(!giulia_pcie_phy_stop(&p, GIULIA_PHY_STOP_REQUIRED));
        assert(f.pipe_off == 1); restored(&f, &p);
    }
    setup(&f, &p);
    f.pcs_first = 1; f.ready_after = 2;
    f.status_override_at = 4; f.status_override = 0x40u;
    /* One failed confirmation consumes the current poll, not a new start. */
    assert(!giulia_pcie_phy_start(&p, stock, &bus));
    assert(p.state == GIULIA_PHY_READY && p.polls == 4 && f.status_reads == 6);
    assert(f.read_at_us[2] == 1400 && f.read_at_us[3] == 1400);
    assert(f.read_at_us[4] == 1600 && f.read_at_us[5] == 1600 && f.elapsed_us == 1600);
    assert(f.pipe_on == 1 && !f.pipe_off && f.pipe_live && !f.reset_writes_after_poll);
    assert(!giulia_pcie_phy_stop(&p, GIULIA_PHY_STOP_REQUIRED));
    assert(f.pipe_off == 1); restored(&f, &p);

    setup(&f, &p);
    f.pcs_first = 1; f.flap_status = 1;
    /* Every first sample is clear and every immediate recheck is busy. */
    assert(giulia_pcie_phy_start(&p, stock, &bus) == GIULIA_PHY_TIMEOUT);
    assert(p.failure == GIULIA_PHY_TIMEOUT && p.last_status == 0x40u);
    assert(p.polls == 51 && f.status_reads == 102 && f.elapsed_us == 11000);
    for (unsigned j = 0; j < 51; ++j) {
        assert(f.read_at_us[2u * j] == 1000u + 200u * j);
        assert(f.read_at_us[2u * j + 1u] == f.read_at_us[2u * j]);
    }
    assert(f.pipe_on == 1 && f.pipe_off == 1); restored(&f, &p);

    setup(&f, &p);
    f.pcs_first = 1; f.ready_after = 50;
    f.status_override_at = 52; f.status_override = 0x40u;
    /* First clear on the last poll still needs confirmation, not extra time. */
    assert(giulia_pcie_phy_start(&p, stock, &bus) == GIULIA_PHY_TIMEOUT);
    assert(p.failure == GIULIA_PHY_TIMEOUT && p.last_status == 0x40u);
    assert(p.polls == 51 && f.status_reads == 52 && f.elapsed_us == 11000);
    assert(f.read_at_us[50] == 11000 && f.read_at_us[51] == 11000);
    assert(f.pipe_on == 1 && f.pipe_off == 1); restored(&f, &p);

    for (unsigned k = 0; k < 2; ++k) {
        setup(&f, &p);
        f.pcs_first = 1; f.ready_after = 2;
        f.status_override_at = 3u + k; f.status_override = UINT32_MAX;
        /* An unreadable first sample or confirmation must abort immediately. */
        assert(giulia_pcie_phy_start(&p, stock, &bus) == GIULIA_PHY_READBACK);
        assert(p.failure == GIULIA_PHY_READBACK && p.last_status == UINT32_MAX);
        assert(p.polls == 3 && f.status_reads == 3u + k && f.elapsed_us == 1400);
        assert(f.read_at_us[2u + k] == 1400);
        assert(f.pipe_on == 1 && f.pipe_off == 1); restored(&f, &p);
    }
    setup(&f, &p);
    f.pcs_first = 1; f.ready_after = 51;
    /* No candidate saw clear: no 52nd access at or after the deadline. */
    assert(giulia_pcie_phy_start(&p, stock, &bus) == GIULIA_PHY_TIMEOUT);
    assert(f.status_reads == 51 && f.elapsed_us == 11000 && f.pipe_off == 1);
    restored(&f, &p);
    setup(&f, &p);
    f.status_override_at = 4; f.status_override = UINT32_MAX;
    /* Upstream PIPE-first still ends on its first clear third sample. */
    assert(!giulia_pcie_phy_start(&p, GIULIA_PHY_START_REQUIRED, &bus));
    assert(f.status_reads == 3 && f.elapsed_us == 1400 && p.state == GIULIA_PHY_READY);
    assert(!giulia_pcie_phy_stop(&p, GIULIA_PHY_STOP_REQUIRED)); restored(&f, &p);
    puts("PASS: PCS-first PHYSTATUS recovery after relapse stays within original deadline");
}
static void test_cesta_start_failure_retention(void)
{
    struct fixture f;
    struct giulia_pcie_phy p;
    struct giulia_pcie_phy_io bus = io(&f);
    const uint32_t base = GIULIA_PHY_START_REQUIRED | GIULIA_PHY_RETAIN_ON_START_FAILURE;
    const uint32_t retained = base | GIULIA_PHY_PCS_BEFORE_PIPE;
    setup(&f, &p);
    assert(giulia_pcie_phy_start(&p, base, &bus) == GIULIA_PHY_PREREQUISITE);
    assert(!f.reads && !f.writes && !f.pipe_on);

    const int errors[] = { GIULIA_PHY_READBACK, GIULIA_PHY_PIPE, GIULIA_PHY_READBACK,
        GIULIA_PHY_TIMEOUT };
    for (unsigned n = 0; n < sizeof(errors) / sizeof(errors[0]); ++n) {
        setup(&f, &p); f.pcs_first = 1;
        if (n == 0) f.ignore_write = START;
        if (n == 1) f.pipe_on_fail = 1;
        if (n == 2) { f.status_override_at = 2; f.status_override = UINT32_MAX; f.ready_after = 0; }
        if (n == 3) f.ready_after = 1000;
        assert(giulia_pcie_phy_start(&p, retained, &bus) == errors[n]);
        assert(p.state == GIULIA_PHY_RESET_REQUIRED && p.failure == errors[n]);
        assert(p.saved_count == 112 && p.pipe_owned == (n != 0) && f.pipe_live == (n != 0));
        assert((f.regs[POWER / 4] & 3u) == 3u && f.regs[0x1640 / 4] == 9);
        assert(f.pipe_on == (n != 0) && !f.pipe_off && !f.reset_writes_after_poll);
        if (n == 2) assert(p.last_status == UINT32_MAX && f.status_reads == 2);
        if (n == 3) assert(p.polls == 51 && f.elapsed_us == 11000);
        const unsigned before = f.reads + f.writes;
        assert(giulia_pcie_phy_stop(&p, GIULIA_PHY_STOP_REQUIRED) == GIULIA_PHY_STATE);
        assert(giulia_pcie_phy_start(&p, retained, &bus) == GIULIA_PHY_STATE);
        assert(f.reads + f.writes == before && !f.pipe_off && p.failure == errors[n]);
    }
    puts("PASS: early CESTA PHY failures preserve snapshot and PIPE until cold reset");
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
    test_stock_ready_confirmation();
    test_cesta_start_failure_retention();
    puts("SM8650 QMP PHY: tuning, PIPE ordering, timeout and restore tests passed");
    return 0;
}
