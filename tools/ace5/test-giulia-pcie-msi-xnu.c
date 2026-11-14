/* SPDX-License-Identifier: BSD-2-Clause */
/* Real XNU adapter with fake core/GIC/CPU services: no hardware or XNU SDK. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int kern_return_t;
typedef int boolean_t;
#define KERN_SUCCESS 0
#define FALSE 0
#define GIULIA_PCIE_MSI_XNU_HOST_TEST 1

static int current_cpu = 1;
static unsigned preemption, mmio_checks;
static boolean_t local_interrupts = 1;
static char events[256];
static unsigned event_count;
static int attach_failure, prepare_failure, arm_failure, prepare_fail_fault;
static unsigned stop_failures;
static unsigned core_calls, gic_calls, callbacks;
static bool gic_masked = true;
static void (*installed_handler)(void *, uint32_t);
static void *installed_context;
static uint32_t dbi_memory[0x900 / 4], config_memory[0x100 / 4];

static void event(char c)
{
    assert(event_count + 1 < sizeof(events));
    events[event_count++] = c;
    events[event_count] = 0;
}
static void expect_events(const char *expected)
{
    if (strcmp(events, expected)) {
        fprintf(stderr, "events: got %s, expected %s\n", events, expected);
        assert(0);
    }
    event_count = 0;
    events[0] = 0;
}
static void disable_preemption(void) { ++preemption; }
static void enable_preemption(void) { assert(preemption); --preemption; }
static boolean_t ml_set_interrupts_enabled(boolean_t enabled)
{
    boolean_t old = local_interrupts;
    assert(preemption);
    local_interrupts = enabled;
    return old;
}
static int cpu_number(void) { return current_cpu; }
static void critical(void) { assert(preemption && !local_interrupts); }

#include "../../xnu/files/bsd/dev/giulia_pcie_msi_xnu.c"

kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge,
    void (*handler)(void *, uint32_t), void *context)
{
    struct giulia_pcie_msi_xnu *x = context;
    critical(); event('A'); ++gic_calls;
    assert(intid == 173 && !edge && handler && x->owned);
    assert(x->core.state == GIULIA_MSI_PREPARED);
    if (attach_failure) return -1;
    assert(!installed_handler);
    installed_handler = handler;
    installed_context = context;
    gic_masked = false; /* The real attach API enables the SPI. */
    return KERN_SUCCESS;
}
void gic_v3_spi_mask(uint32_t intid)
{
    assert(!local_interrupts && intid == 173);
    event('M'); ++gic_calls; gic_masked = true;
}
void gic_v3_spi_unmask(uint32_t intid)
{
    struct giulia_pcie_msi_xnu *x = installed_context;
    critical(); event('U'); ++gic_calls;
    assert(intid == 173 && installed_handler && x->attached);
    assert(x->core.state == GIULIA_MSI_ACTIVE);
    gic_masked = false;
}
void gic_v3_spi_detach(uint32_t intid)
{
    critical(); event('D'); ++gic_calls;
    assert(intid == 173 && installed_handler && gic_masked);
    installed_handler = NULL;
    installed_context = NULL;
}

int giulia_pcie_msi_prepare(struct giulia_pcie_msi *m,
    const struct giulia_pcie_msi_config *config,
    const struct giulia_pcie_msi_io *io)
{
    critical(); event('P'); ++core_calls;
    if ((config->prerequisites & GIULIA_MSI_REQUIRED) != GIULIA_MSI_REQUIRED)
        return GIULIA_MSI_PREREQUISITE;
    if (prepare_failure) {
        if (prepare_fail_fault) m->state = GIULIA_MSI_FAULT;
        return prepare_failure;
    }
    m->io = *io;
    m->state = GIULIA_MSI_PREPARED;
    m->valid_mask = UINT32_MAX;
    /* Exercise the adapter's real MMIO callbacks on aligned host arrays. */
    io->write_dbi32(io->cookie, 0x820, 0x60100000);
    assert(dbi_memory[0x820 / 4] == 0x60100000);
    assert(io->read_dbi32(io->cookie, 0x820) == 0x60100000);
    config_memory[4 / 4] = 0xa55a55aa;
    io->write_cfg16(io->cookie, 4, 0x0406);
    assert(config_memory[4 / 4] == 0xa55a0406);
    assert(io->read_cfg16(io->cookie, 6) == 0xa55a);
    config_memory[0x50 / 4] = 0xabcd7005;
    io->write_cfg16(io->cookie, 0x52, 0x15a);
    assert(io->read_cfg16(io->cookie, 0x52) == 0x15a);
    assert(config_memory[0x50 / 4] == 0x015a7005);
    io->write_cfg32(io->cookie, 0x54, 0x60100000);
    assert(config_memory[0x54 / 4] == 0x60100000);
    assert(io->read_cfg32(io->cookie, 0x54) == 0x60100000);
    io->barrier(io->cookie);
    ++mmio_checks;
    return GIULIA_MSI_OK;
}
int giulia_pcie_msi_arm(struct giulia_pcie_msi *m, uint32_t intid,
    uint32_t enable_mask)
{
    critical(); event('R'); ++core_calls;
    assert(intid == 173 && installed_handler && gic_masked);
    assert(m->state == GIULIA_MSI_PREPARED);
    if (arm_failure) {
        m->state = GIULIA_MSI_FAULT;
        return arm_failure;
    }
    m->state = GIULIA_MSI_ACTIVE;
    m->enabled_mask = enable_mask;
    return GIULIA_MSI_OK;
}
int giulia_pcie_msi_mask(struct giulia_pcie_msi *m, uint32_t bits)
{
    critical(); event('m'); ++core_calls;
    assert(m->state == GIULIA_MSI_ACTIVE);
    m->enabled_mask &= ~bits;
    return GIULIA_MSI_OK;
}
int giulia_pcie_msi_unmask(struct giulia_pcie_msi *m, uint32_t bits)
{
    critical(); event('u'); ++core_calls;
    assert(m->state == GIULIA_MSI_ACTIVE);
    m->enabled_mask |= bits;
    return GIULIA_MSI_OK;
}
uint32_t giulia_pcie_msi_dispatch(struct giulia_pcie_msi *m,
    void (*deliver)(void *, unsigned), void *cookie)
{
    assert(!local_interrupts && m->state == GIULIA_MSI_ACTIVE);
    event('I'); ++core_calls;
    deliver(cookie, 3);
    return 1u << 3;
}
int giulia_pcie_msi_stop(struct giulia_pcie_msi *m)
{
    critical(); event('S'); ++core_calls;
    if (stop_failures) {
        --stop_failures;
        m->state = GIULIA_MSI_FAULT;
        return GIULIA_MSI_READBACK;
    }
    m->state = GIULIA_MSI_STOPPED;
    return GIULIA_MSI_OK;
}

static void receive(void *cookie, unsigned vector)
{
    struct giulia_pcie_msi_xnu *x = cookie;
    assert(vector == 3 && x->in_handler && !local_interrupts);
    ++callbacks; event('F');
    /* No detach/wait-in-own-handler deadlock, and no object replacement. */
    assert(giulia_pcie_msi_xnu_stop(x) == GIULIA_MSI_STATE);
    assert(giulia_pcie_msi_xnu_mask(x, 1u << 3) == GIULIA_MSI_OK);
}
static void reset_fixture(void)
{
    assert(!preemption && local_interrupts && !installed_handler);
    event_count = core_calls = gic_calls = callbacks = mmio_checks = 0;
    events[0] = 0;
    attach_failure = prepare_failure = arm_failure = prepare_fail_fault = 0;
    stop_failures = 0;
    current_cpu = 1;
    gic_masked = true;
    memset(dbi_memory, 0, sizeof(dbi_memory));
    memset(config_memory, 0, sizeof(config_memory));
}
static int start(struct giulia_pcie_msi_xnu *x, uint32_t prerequisites)
{
    struct giulia_pcie_msi_config config = {
        .target = 0x60100000, .prerequisites = prerequisites,
        .capability = 0x50, .vectors = 32
    };
    return giulia_pcie_msi_xnu_start(x, &config,
        (uintptr_t)dbi_memory, sizeof(dbi_memory),
        (uintptr_t)config_memory, sizeof(config_memory), UINT32_MAX, receive, x);
}
static void fire(uint32_t intid)
{
    assert(installed_handler && local_interrupts && !preemption);
    local_interrupts = 0;
    installed_handler(installed_context, intid);
    local_interrupts = 1;
    assert(!preemption);
}

int main(void)
{
    struct giulia_pcie_msi_xnu x = {0};
    uintptr_t saved_dbi, saved_cfg;
    unsigned before;
    reset_fixture();
    /* A zero-initialized, unstarted adapter does not touch core/GIC/MMIO. */
    assert(giulia_pcie_msi_xnu_stop(&x) == 0);
    assert(giulia_pcie_msi_xnu_mask(&x, 1) == GIULIA_MSI_STATE);
    assert(giulia_pcie_msi_xnu_unmask(&x, 1) == GIULIA_MSI_STATE);
    expect_events("");
    assert(!core_calls && !gic_calls && !mmio_checks);
    assert(start(&x, 0) == GIULIA_MSI_PREREQUISITE);
    expect_events("P");
    assert(!x.owned && !x.attached && !gic_calls && !mmio_checks);

    assert(start(&x, GIULIA_MSI_REQUIRED) == 0);
    expect_events("PAMRU");
    assert(x.owned && x.attached && x.owner_cpu == 1 && !gic_masked);
    assert(!preemption && local_interrupts && mmio_checks == 1);
    before = core_calls;
    assert(start(&x, GIULIA_MSI_REQUIRED) == GIULIA_MSI_STATE);
    assert(core_calls == before);
    assert(giulia_pcie_msi_xnu_mask(&x, 1) == 0);
    assert(giulia_pcie_msi_xnu_unmask(&x, 1) == 0);
    expect_events("mu");

    current_cpu = 2;
    before = core_calls;
    assert(giulia_pcie_msi_xnu_mask(&x, 1) == GIULIA_MSI_STATE);
    assert(giulia_pcie_msi_xnu_unmask(&x, 1) == GIULIA_MSI_STATE);
    assert(giulia_pcie_msi_xnu_stop(&x) == GIULIA_MSI_STATE);
    expect_events("");
    assert(core_calls == before && x.owned && x.attached);
    fire(173); /* Defensive wrong-CPU IRQ masking, no driver delivery. */
    expect_events("M");
    assert(core_calls == before && !callbacks);
    current_cpu = 1;
    fire(174);
    expect_events("");
    fire(173);
    expect_events("IFm");
    assert(x.interrupts == 1 && x.delivered == 1 && callbacks == 1);
    assert(!x.in_handler && x.owned && x.attached);

    saved_dbi = x.dbi; saved_cfg = x.cfg;
    stop_failures = 1;
    assert(giulia_pcie_msi_xnu_stop(&x) == GIULIA_MSI_READBACK);
    expect_events("SM");
    assert(x.owned && x.attached && installed_handler && gic_masked);
    assert(x.dbi == saved_dbi && x.cfg == saved_cfg);
    assert(giulia_pcie_msi_xnu_stop(&x) == 0);
    expect_events("SMD");
    assert(!x.owned && !x.attached && !installed_handler);
    assert(giulia_pcie_msi_xnu_stop(&x) == 0);
    expect_events("");

    memset(&x, 0, sizeof(x)); reset_fixture();
    attach_failure = 1;
    assert(start(&x, GIULIA_MSI_REQUIRED) == GIULIA_MSI_PREREQUISITE);
    expect_events("PAS"); /* Never mask/detach the slot we failed to acquire. */
    assert(!x.owned && !x.attached && !installed_handler && gic_calls == 1);

    memset(&x, 0, sizeof(x)); reset_fixture();
    attach_failure = 1; stop_failures = 1;
    assert(start(&x, GIULIA_MSI_REQUIRED) == GIULIA_MSI_PREREQUISITE);
    expect_events("PAS");
    assert(x.owned && !x.attached && !installed_handler);
    assert(giulia_pcie_msi_xnu_stop(&x) == 0);
    expect_events("S");
    assert(!x.owned && gic_calls == 1);

    memset(&x, 0, sizeof(x)); reset_fixture();
    arm_failure = GIULIA_MSI_READBACK;
    assert(start(&x, GIULIA_MSI_REQUIRED) == GIULIA_MSI_READBACK);
    expect_events("PAMRSMD");
    assert(!x.owned && !x.attached && !installed_handler);

    memset(&x, 0, sizeof(x)); reset_fixture();
    arm_failure = GIULIA_MSI_READBACK; stop_failures = 1;
    assert(start(&x, GIULIA_MSI_REQUIRED) == GIULIA_MSI_READBACK);
    expect_events("PAMRSM");
    assert(x.owned && x.attached && installed_handler && gic_masked);
    assert(giulia_pcie_msi_xnu_stop(&x) == 0);
    expect_events("SMD");
    assert(!x.owned && !x.attached && !installed_handler);

    memset(&x, 0, sizeof(x)); reset_fixture();
    prepare_failure = GIULIA_MSI_CAPABILITY;
    assert(start(&x, GIULIA_MSI_REQUIRED) == GIULIA_MSI_CAPABILITY);
    expect_events("P");
    assert(!x.owned && !x.attached && !gic_calls && !mmio_checks);
    memset(&x, 0, sizeof(x)); reset_fixture();
    prepare_failure = GIULIA_MSI_READBACK; prepare_fail_fault = 1;
    assert(start(&x, GIULIA_MSI_REQUIRED) == GIULIA_MSI_READBACK);
    expect_events("P");
    assert(x.owned && !x.attached && !gic_calls);
    assert(giulia_pcie_msi_xnu_stop(&x) == 0);
    expect_events("S");
    assert(!x.owned && !x.attached && !gic_calls);
    assert(!preemption && local_interrupts);
    puts("giulia_pcie_msi_xnu: lifecycle, IRQ ordering and retry tests passed");
    return 0;
}
