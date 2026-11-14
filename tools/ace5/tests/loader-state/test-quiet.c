/* SPDX-License-Identifier: BSD-2-Clause */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "loader_firmware_quiet.h"
static struct loader_firmware_quiet firmware_quiet;
static int firmware_quiet_enabled, wdt_armed, masked, mode;
static volatile uint64_t other_cpu_done;
static unsigned flushes, clean_acks, restored, pets;
static uint64_t ticks;
#define CNTVCT_EL0 0
#define READ_SYSREG(x) (++ticks)
static uint64_t seconds_since(uint64_t start) { (void)start; return ticks; }
static int observer_watchdog_within_budget(uint64_t elapsed, uint64_t budget)
{ return !budget || elapsed < budget + 60; }
static void wdt_pet(void) { assert(masked); pets++; }
static void quiet_interrupts_mask(uint64_t *d, uint64_t *g)
{ assert(!masked); *d = 7; *g = 9; masked = 1; }
static void quiet_interrupts_restore(uint64_t d, uint64_t g)
{ assert(d == 7 && g == 9 && masked && !firmware_quiet.ack); masked = 0; restored++; }
static void clean_dcache(uint64_t a, uint64_t b)
{
    uint64_t base = (uint64_t)(uintptr_t)&firmware_quiet;
    assert(masked && a == base + 128 && b == base + 256);
    if (firmware_quiet.ack) assert(firmware_quiet.active && firmware_quiet.ack == firmware_quiet.request);
    else assert(!firmware_quiet.active);
    clean_acks++;
}
static void flush_dcache(uint64_t a, uint64_t b)
{
    uint64_t base = (uint64_t)(uintptr_t)&firmware_quiet;
    if (a == base) {
        assert(b == base + 128); flushes++;
        if (mode == 1 && masked && !firmware_quiet.ack) firmware_quiet.request = 0;
        if (firmware_quiet.ack) {
            assert(masked);
            ticks += 6;
            if (flushes > 5) {
                if (mode == 2) other_cpu_done = 1;
                else firmware_quiet.request = 0;
            }
        }
    } else assert(a == (uint64_t)(uintptr_t)&other_cpu_done && b == a + 8);
}
#include "quiet-observer.inc"
static void reset(void)
{
    firmware_quiet = (struct loader_firmware_quiet){0};
    firmware_quiet_enabled = 1; wdt_armed = 1; other_cpu_done = 0;
    mode = masked = flushes = clean_acks = restored = pets = 0; ticks = 10;
}
int main(void)
{
    uint64_t last = 0;
    reset(); firmware_quiet_enabled = 0;
    assert(!observer_firmware_quiet(0, 90, &last) && !flushes);
    reset(); assert(!observer_firmware_quiet(0, 90, &last) && !restored);
    reset(); firmware_quiet.request = 42;
    assert(observer_firmware_quiet(0, 90, &last) == 1);
    assert(clean_acks == 2 && restored == 1 && !masked && pets && !firmware_quiet.ack);
    reset(); mode = 1; firmware_quiet.request = 3;
    assert(observer_firmware_quiet(0, 90, &last) == 1 && restored == 1 && !clean_acks);
    reset(); mode = 2; firmware_quiet.request = 5;
    assert(observer_firmware_quiet(0, 90, &last) == 0 && other_cpu_done && restored == 1 && !firmware_quiet.ack);
    reset(); firmware_quiet.request = 6; other_cpu_done = 1;
    assert(!observer_firmware_quiet(0, 90, &last) && !restored);
    puts("PASS: actual loader firmware quiet ACK, cancellation, kernel return and interrupt restore");
    return 0;
}
