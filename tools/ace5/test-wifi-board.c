/* SPDX-License-Identifier: BSD-2-Clause */
/* Link this fixture with the production giulia_wifi_board.c only. Leaf cores
 * are mocked here; their real register implementations have separate tests. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "giulia_wifi_board.h"

struct mock {
    struct giulia_wifi_board *board;
    char trace[128];
    unsigned trace_count, external_calls, external_fail_at, external_persistent;
    unsigned phy_reads, phy_writes, phy_barriers, phy_delays;
    char start_fail, stop_fail, reject_start;
    unsigned phy_auto_cleanup;
};
static struct mock *current;
static int event(char c)
{
    struct mock *m = current;
    assert(m->trace_count < sizeof(m->trace) - 1);
    m->trace[m->trace_count++] = c;
    if (m->start_fail == c) { m->start_fail = 0; return -55; }
    if (m->stop_fail == c) { m->stop_fail = 0; return -66; }
    return 0;
}
static void required(uint32_t flags, uint32_t need)
{ assert((flags & need) == need); }
static int external(void *p, uint32_t need)
{
    struct mock *m = p;
    assert(m == current);
    assert(need == GIULIA_WBOARD_START_REQUIRED || need == GIULIA_WBOARD_STOP_REQUIRED);
    m->external_calls++;
    if (m->external_fail_at && (m->external_calls == m->external_fail_at ||
        (m->external_persistent && m->external_calls >= m->external_fail_at)))
        return -91;
    return 0;
}
static uint32_t unused_read(void *p, uint32_t o)
{ (void)p; (void)o; assert(!"unexpected leaf MMIO"); return 0; }
static void unused_write(void *p, uint32_t o, uint32_t v)
{ (void)p; (void)o; (void)v; assert(!"unexpected leaf MMIO"); }
static void unused_barrier(void *p) { (void)p; assert(!"unexpected leaf MMIO"); }
static void unused_delay(void *p, uint32_t us)
{ (void)p; (void)us; assert(!"unexpected leaf MMIO"); }
static uint32_t unused_block_read(void *p, unsigned r, uint32_t o)
{ (void)r; return unused_read(p, o); }
static void unused_block_write(void *p, unsigned r, uint32_t o, uint32_t v)
{ (void)r; unused_write(p, o, v); }
static uint16_t unused_block_read16(void *p, unsigned r, uint32_t o)
{ (void)r; return (uint16_t)unused_read(p, o); }
static void unused_block_write16(void *p, unsigned r, uint32_t o, uint16_t v)
{ (void)r; unused_write(p, o, v); }
static uint32_t phy_read(void *p, uint32_t o)
{ struct mock *m = p; assert(m == current && o == 0x44); m->phy_reads++; return 0x1234; }
static void phy_write(void *p, uint32_t o, uint32_t v)
{ struct mock *m = p; assert(m == current && o == 0x44 && v == 0x11223344); m->phy_writes++; }
static void phy_barrier(void *p)
{ struct mock *m = p; assert(m == current); m->phy_barriers++; }
static void phy_delay(void *p, uint32_t us)
{ struct mock *m = p; assert(m == current && us == 1000); m->phy_delays++; }
static struct giulia_wifi_board_io init(struct mock *m, struct giulia_wifi_board *b)
{
    struct giulia_wifi_board_io io;
    memset(m, 0, sizeof(*m)); memset(b, 0, sizeof(*b)); memset(&io, 0, sizeof(io));
    current = m; m->board = b;
    io.gpio = (struct giulia_wifi_gpio_io){m, unused_read, unused_write,
        unused_barrier, unused_delay};
    io.power = (struct giulia_pcie_power_io){m, unused_block_read,
        unused_block_write, unused_barrier, unused_delay};
    io.link = (struct giulia_pcie_link_io){m, unused_block_read,
        unused_block_write, unused_block_read16, unused_block_write16,
        unused_barrier, unused_delay};
    io.phy = (struct giulia_wifi_board_phy_io){m, phy_read, phy_write, phy_barrier, phy_delay};
    io.external_cookie = m; io.validate_external = external;
    return io;
}

int giulia_wifi_gpio_begin(struct giulia_wifi_gpio *g, uint32_t flags,
    const struct giulia_wifi_gpio_io *io)
{
    int rc;
    assert(g == &current->board->gpio && io->cookie == current);
    required(flags, GIULIA_WGPIO_BEGIN_REQUIRED);
    rc = event('G');
    if (current->reject_start == 'G') return -55;
    g->io = *io; g->state = rc ? GIULIA_WGPIO_FAULT : GIULIA_WGPIO_HELD;
    g->dirty = 15;
    return rc;
}
int giulia_wifi_gpio_wlan_on(struct giulia_wifi_gpio *g, uint32_t flags)
{
    int rc;
    assert(g->state == GIULIA_WGPIO_HELD);
    required(flags, GIULIA_WGPIO_ON_REQUIRED);
    rc = event('W'); g->state = rc ? GIULIA_WGPIO_FAULT : GIULIA_WGPIO_ON;
    return rc;
}
int giulia_wifi_gpio_perst_release(struct giulia_wifi_gpio *g, uint32_t flags)
{
    int rc;
    assert(g->state == GIULIA_WGPIO_ON && !current->board->perst_isolated);
    required(flags, GIULIA_WGPIO_RELEASE_REQUIRED);
    rc = event('R'); g->state = rc ? GIULIA_WGPIO_FAULT : GIULIA_WGPIO_RELEASED;
    return rc;
}
int giulia_wifi_gpio_perst_assert(struct giulia_wifi_gpio *g, uint32_t flags)
{
    int rc;
    assert(g->state != GIULIA_WGPIO_IDLE && g->state != GIULIA_WGPIO_STOPPED);
    required(flags, GIULIA_WGPIO_BEGIN_REQUIRED);
    rc = event('A');
    if (rc) g->state = GIULIA_WGPIO_FAULT;
    else if (g->state == GIULIA_WGPIO_RELEASED) g->state = GIULIA_WGPIO_ON;
    return rc;
}
int giulia_wifi_gpio_stop(struct giulia_wifi_gpio *g, uint32_t flags)
{
    int rc;
    required(flags, GIULIA_WGPIO_STOP_REQUIRED);
    assert(current->board->owned == GIULIA_WBOARD_OWNS_GPIO);
    assert(current->board->perst_isolated && !current->board->host_clocks_ready);
    rc = event('g'); g->state = rc ? GIULIA_WGPIO_FAULT : GIULIA_WGPIO_STOPPED;
    return rc;
}
int giulia_pcie_power_start(struct giulia_pcie_power *p,
    const struct giulia_pcie_power_io *io, uint32_t flags)
{
    int rc;
    assert(p == &current->board->power && io->cookie == current);
    required(flags, GIULIA_POWER_REQUIRED);
    assert(current->board->perst_isolated && current->board->gpio.state == GIULIA_WGPIO_ON);
    rc = event('P');
    if (current->reject_start == 'P') return -55;
    p->io = *io; p->state = rc ? GIULIA_POWER_FAULT : GIULIA_POWER_READY;
    return rc;
}
int giulia_pcie_power_pipe_enable(struct giulia_pcie_power *p)
{
    int rc;
    assert(p == &current->board->power && p->state == GIULIA_POWER_READY);
    assert(current->board->perst_isolated && !current->board->link_may_run);
    rc = event('E'); p->state = rc ? GIULIA_POWER_FAULT : GIULIA_POWER_PIPE_ON;
    return rc;
}
int giulia_pcie_power_pipe_disable(struct giulia_pcie_power *p)
{
    int rc;
    assert(p == &current->board->power && current->board->host_clocks_ready);
    assert(current->board->perst_isolated && !current->board->link_may_run);
    rc = event('e');
    if (rc) p->state = GIULIA_POWER_FAULT;
    else if (p->state == GIULIA_POWER_PIPE_ON) p->state = GIULIA_POWER_READY;
    return rc;
}
int giulia_pcie_power_stop(struct giulia_pcie_power *p, uint32_t flags)
{
    int rc;
    required(flags, GIULIA_POWER_STOP_REQUIRED);
    assert(!(current->board->owned & (GIULIA_WBOARD_OWNS_LINK | GIULIA_WBOARD_OWNS_PHY)));
    rc = event('p'); p->state = rc ? GIULIA_POWER_FAULT : GIULIA_POWER_STOPPED;
    return rc;
}
int giulia_pcie_link_prepare(struct giulia_pcie_link *l, uint32_t flags,
    const struct giulia_pcie_link_io *io)
{
    int rc;
    assert(l == &current->board->link && io->cookie == current);
    required(flags, GIULIA_LINK_PREPARE_REQUIRED);
    assert(!(flags & (GIULIA_LINK_PHY_READY | GIULIA_LINK_PERST_RELEASED_SETTLED)));
    rc = event('L');
    if (current->reject_start == 'L') { l->state = GIULIA_LINK_STOPPED; return -55; }
    l->io = *io; l->parf_owned = l->core_owned = 1;
    l->state = rc ? GIULIA_LINK_FAULT : GIULIA_LINK_PREPARED;
    return rc;
}
int giulia_pcie_link_train(struct giulia_pcie_link *l, uint32_t flags)
{
    int rc;
    required(flags, GIULIA_LINK_TRAIN_REQUIRED);
    assert(!(flags & GIULIA_LINK_PERST_ASSERTED));
    assert(current->board->link_may_run && !current->board->perst_isolated);
    rc = event('T'); l->state = rc ? GIULIA_LINK_FAULT : GIULIA_LINK_READY;
    return rc;
}
int giulia_pcie_link_mem_enable(struct giulia_pcie_link *l, uint32_t flags)
{
    int rc;
    required(flags, GIULIA_LINK_MEMORY_REQUIRED); assert(l->state == GIULIA_LINK_READY);
    rc = event('M'); l->memory_owned = l->memory_enabled = 1;
    if (rc) l->state = GIULIA_LINK_FAULT;
    return rc;
}
int giulia_pcie_link_quiesce(struct giulia_pcie_link *l, uint32_t flags)
{
    int rc;
    required(flags, GIULIA_LINK_QUIESCE_REQUIRED);
    assert(current->board->perst_isolated && current->board->host_clocks_ready);
    assert(!(flags & GIULIA_LINK_PERST_RELEASED_SETTLED));
    rc = event('l'); l->state = rc ? GIULIA_LINK_FAULT : GIULIA_LINK_QUIESCED;
    if (!rc) l->memory_enabled = 0;
    return rc;
}
int giulia_pcie_link_stop(struct giulia_pcie_link *l, uint32_t flags)
{
    int rc;
    required(flags, GIULIA_LINK_STOP_REQUIRED);
    assert(current->board->perst_isolated && current->board->host_clocks_ready);
    assert(!(current->board->owned & GIULIA_WBOARD_OWNS_PHY));
    assert(l->state == GIULIA_LINK_QUIESCED);
    rc = event('s'); l->state = rc ? GIULIA_LINK_FAULT : GIULIA_LINK_STOPPED;
    if (!rc) l->parf_owned = l->core_owned = l->memory_owned = l->memory_enabled = 0;
    return rc;
}
int giulia_pcie_phy_start(struct giulia_pcie_phy *p, uint32_t flags,
    const struct giulia_pcie_phy_io *io)
{
    int rc;
    required(flags, GIULIA_PHY_START_REQUIRED);
    assert(p == &current->board->phy && io->cookie == current->board);
    rc = event('Y');
    if (current->reject_start == 'Y') return -55;
    p->io = *io; p->state = GIULIA_PHY_OWNED;
    /* Verify all bound callbacks forward the original PHY MMIO cookie. */
    assert(io->read32(io->cookie, 0x44) == 0x1234);
    io->write32(io->cookie, 0x44, 0x11223344);
    io->barrier(io->cookie); io->delay_us(io->cookie, 1000);
    if (!rc) {
        p->pipe_owned = 1;
        rc = io->pipe_enable(io->cookie);
    }
    if (rc) {
        p->state = GIULIA_PHY_FAULT;
        if (current->phy_auto_cleanup) {
            if (p->pipe_owned && io->pipe_disable(io->cookie)) return GIULIA_PHY_PIPE;
            p->pipe_owned = 0; p->state = GIULIA_PHY_STOPPED;
        }
        return GIULIA_PHY_PIPE;
    }
    p->state = GIULIA_PHY_READY;
    return 0;
}
int giulia_pcie_phy_stop(struct giulia_pcie_phy *p, uint32_t flags)
{
    int rc;
    required(flags, GIULIA_PHY_STOP_REQUIRED);
    assert(current->board->link.state == GIULIA_LINK_QUIESCED);
    rc = event('y');
    if (!rc && p->pipe_owned) rc = p->io.pipe_disable(p->io.cookie);
    if (rc) { p->state = GIULIA_PHY_FAULT; return GIULIA_PHY_PIPE; }
    p->pipe_owned = 0; p->state = GIULIA_PHY_STOPPED;
    return 0;
}

static void test_default_and_success(void)
{
    struct mock m; struct giulia_wifi_board b;
    struct giulia_wifi_board_io io = init(&m, &b);
    unsigned calls;
    assert(giulia_wifi_board_stop(&b, 0) == 0 && !m.external_calls);
    for (unsigned bit = 0; bit < 8; bit++)
        assert(giulia_wifi_board_start(&b, &io, GIULIA_WBOARD_START_REQUIRED &
            ~(1u << bit)) == GIULIA_WBOARD_PREREQUISITE);
    assert(!m.external_calls && !m.trace_count);
    io.validate_external = NULL;
    assert(giulia_wifi_board_start(&b, &io, GIULIA_WBOARD_START_REQUIRED) == GIULIA_WBOARD_ARGUMENT);
    io.validate_external = external;
    assert(giulia_wifi_board_start(&b, &io, GIULIA_WBOARD_START_REQUIRED) == 0);
    assert(strcmp(m.trace, "GWPLYERTM") == 0);
    assert(b.state == GIULIA_WBOARD_CPU_BAR_READY && b.owned == 15 && b.link.memory_enabled);
    assert(m.phy_reads == 1 && m.phy_writes == 1 && m.phy_barriers == 1 && m.phy_delays == 1);
    calls = m.external_calls;
    assert(giulia_wifi_board_start(&b, &io, GIULIA_WBOARD_START_REQUIRED) == GIULIA_WBOARD_STATE);
    assert(giulia_wifi_board_stop(&b, GIULIA_WBOARD_STOP_REQUIRED &
        ~GIULIA_WBOARD_USERS_QUIESCED) == GIULIA_WBOARD_PREREQUISITE);
    assert(m.external_calls == calls);
    assert(giulia_wifi_board_stop(&b, GIULIA_WBOARD_STOP_REQUIRED) == 0);
    assert(strcmp(m.trace, "GWPLYERTMAlyespg") == 0);
    assert(b.state == GIULIA_WBOARD_STOPPED && !b.owned);
    calls = m.external_calls;
    assert(giulia_wifi_board_stop(&b, 0) == 0 && m.external_calls == calls);
}
static void test_every_start_failure(void)
{
    static const char failures[] = "GWPLYERTM";
    static const uint32_t stages[] = {GIULIA_WBOARD_STAGE_GPIO_BEGIN,
        GIULIA_WBOARD_STAGE_WLAN_ON, GIULIA_WBOARD_STAGE_POWER,
        GIULIA_WBOARD_STAGE_LINK_PREPARE, GIULIA_WBOARD_STAGE_PHY,
        GIULIA_WBOARD_STAGE_PHY, GIULIA_WBOARD_STAGE_PERST_RELEASE,
        GIULIA_WBOARD_STAGE_LINK_TRAIN, GIULIA_WBOARD_STAGE_MEMORY};
    for (unsigned i = 0; failures[i]; i++) {
        struct mock m; struct giulia_wifi_board b;
        struct giulia_wifi_board_io io = init(&m, &b);
        m.start_fail = failures[i];
        assert(giulia_wifi_board_start(&b, &io, GIULIA_WBOARD_START_REQUIRED) == GIULIA_WBOARD_SUBSYSTEM);
        assert(b.failed_stage == stages[i] && b.driver_error != 0);
        assert(b.state == GIULIA_WBOARD_STOPPED && !b.owned && !b.cleanup_error);
        assert(strchr(m.trace, 'A') && m.trace[m.trace_count - 1] == 'g');
    }
    for (unsigned i = 0; "GPLY"[i]; i++) {
        struct mock m; struct giulia_wifi_board b;
        struct giulia_wifi_board_io io = init(&m, &b);
        m.reject_start = "GPLY"[i];
        assert(giulia_wifi_board_start(&b, &io, GIULIA_WBOARD_START_REQUIRED) == GIULIA_WBOARD_SUBSYSTEM);
        assert(b.state == GIULIA_WBOARD_STOPPED && !b.owned);
        if (m.reject_start == 'G') assert(strcmp(m.trace, "G") == 0);
        if (m.reject_start == 'P') assert(!strchr(m.trace, 'p'));
        if (m.reject_start == 'L') assert(!strchr(m.trace, 'l') && !strchr(m.trace, 's'));
        if (m.reject_start == 'Y') assert(!strchr(m.trace, 'y') && !strchr(m.trace, 'E'));
    }
    {
        struct mock m; struct giulia_wifi_board b;
        struct giulia_wifi_board_io io = init(&m, &b);
        m.start_fail = 'E'; m.phy_auto_cleanup = 1;
        assert(giulia_wifi_board_start(&b, &io, GIULIA_WBOARD_START_REQUIRED) == GIULIA_WBOARD_SUBSYSTEM);
        assert(strcmp(m.trace, "GWPLYEeAlspg") == 0);
        assert(b.state == GIULIA_WBOARD_STOPPED && !b.owned);
    }
}
static void test_every_cleanup_failure(void)
{
    static const char failures[] = "Alyespg";
    static const char *prefix[] = {"A", "Al", "Aly", "Alye", "Alyes", "Alyesp", "Alyespg"};
    static const unsigned owners[] = {15, 15, 15, 15, 7, 3, 1};
    for (unsigned i = 0; failures[i]; i++) {
        struct mock m; struct giulia_wifi_board b;
        struct giulia_wifi_board_io io = init(&m, &b);
        assert(giulia_wifi_board_start(&b, &io, GIULIA_WBOARD_START_REQUIRED) == 0);
        memset(m.trace, 0, sizeof(m.trace)); m.trace_count = 0;
        m.stop_fail = failures[i];
        assert(giulia_wifi_board_stop(&b, GIULIA_WBOARD_STOP_REQUIRED) == GIULIA_WBOARD_SUBSYSTEM);
        assert(strcmp(m.trace, prefix[i]) == 0);
        assert(b.state == GIULIA_WBOARD_FAULT && b.owned == owners[i]);
        assert(b.cleanup_error && b.cleanup_driver_error);
        assert(giulia_wifi_board_stop(&b, GIULIA_WBOARD_STOP_REQUIRED) == 0);
        assert(b.state == GIULIA_WBOARD_STOPPED && !b.owned);
    }
    {
        struct mock m; struct giulia_wifi_board b;
        struct giulia_wifi_board_io io = init(&m, &b);
        m.start_fail = 'T'; m.stop_fail = 'l';
        assert(giulia_wifi_board_start(&b, &io, GIULIA_WBOARD_START_REQUIRED) == GIULIA_WBOARD_SUBSYSTEM);
        assert(b.failed_stage == GIULIA_WBOARD_STAGE_LINK_TRAIN);
        assert(b.cleanup_stage == GIULIA_WBOARD_STAGE_LINK_STOP && b.owned == 15);
        assert(!strchr(m.trace, 'y') && !strchr(m.trace, 'p') && !strchr(m.trace, 'g'));
        assert(giulia_wifi_board_stop(&b, GIULIA_WBOARD_STOP_REQUIRED) == 0);
        assert(b.error == GIULIA_WBOARD_SUBSYSTEM && b.failed_stage == GIULIA_WBOARD_STAGE_LINK_TRAIN);
    }
}
static void test_external_loss(void)
{
    for (unsigned at = 1; at <= 9; at++) {
        struct mock m; struct giulia_wifi_board b;
        struct giulia_wifi_board_io io = init(&m, &b);
        m.external_fail_at = at; m.external_persistent = 1;
        assert(giulia_wifi_board_start(&b, &io, GIULIA_WBOARD_START_REQUIRED) != 0);
        if (at == 1) assert(!m.trace_count && b.state == GIULIA_WBOARD_STOPPED);
        else {
            assert(b.state == GIULIA_WBOARD_FAULT && b.owned);
            assert(!strchr(m.trace, 'A') && !strchr(m.trace, 'p') && !strchr(m.trace, 'g'));
            m.external_fail_at = 0;
            assert(giulia_wifi_board_stop(&b, GIULIA_WBOARD_STOP_REQUIRED) == 0);
            assert(b.state == GIULIA_WBOARD_STOPPED && !b.owned);
        }
    }
    {
        struct mock m; struct giulia_wifi_board b;
        struct giulia_wifi_board_io io = init(&m, &b);
        unsigned count;
        assert(giulia_wifi_board_start(&b, &io, GIULIA_WBOARD_START_REQUIRED) == 0);
        count = m.trace_count; m.external_fail_at = m.external_calls + 1;
        assert(giulia_wifi_board_stop(&b, GIULIA_WBOARD_STOP_REQUIRED) == GIULIA_WBOARD_EXTERNAL);
        assert(m.trace_count == count && b.owned == 15);
        m.external_fail_at = 0;
        assert(giulia_wifi_board_stop(&b, GIULIA_WBOARD_STOP_REQUIRED) == 0);
    }
}
int main(void)
{
    test_default_and_success();
    test_every_start_failure();
    test_every_cleanup_failure();
    test_external_loss();
    puts("wifi-board: real coordinator ordering, bound PIPE, faults and retained ownership passed");
    return 0;
}
