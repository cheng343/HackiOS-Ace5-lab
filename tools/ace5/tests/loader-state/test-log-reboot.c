/* SPDX-License-Identifier: BSD-2-Clause */
/* Simulate watchdog register ownership and CPU1's assembly return flag. */
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "giulia_gunyah_watchdog.h"
#include "loader_firmware_quiet.h"
#define READ_SYSREG(x) ticks
#define PSCI_SYSTEM_RESET 1u
#define print(...) ((void)0)
static struct loader_firmware_quiet firmware_quiet;
static int log_reboot_owned;
static uint64_t state, ticks, bark, bite, other_cpu_done;
static unsigned writes, pets, disables, reset_attempts;
static uint64_t failed_id;
static jmp_buf reset_target;

static uint64_t smc_call(uint64_t id, uint64_t a1, uint64_t a2, uint64_t *r1, uint64_t *r2)
{
    (void)r2;
    if (id == failed_id) return 1;
    if (id == GIULIA_WDT_STATUS) { *r1 = state; return 0; }
    writes++;
    if (id == GIULIA_WDT_SET_TIME) {
        if (a1 != GIULIA_WDT_NO_CHANGE) bark = a1;
        if (a2 != GIULIA_WDT_NO_CHANGE) bite = a2;
        if (state & GIULIA_WDT_ENABLED) assert(bark < bite);
    } else if (id == GIULIA_WDT_CONTROL) {
        state = a1 == 3 ? GIULIA_WDT_ENABLED : 0;
    } else { assert(id == GIULIA_WDT_PET); pets++; }
    return 0;
}
static int quiet_watchdog_disarm(void) { disables++; state = 0; return 0; }
static uint64_t seconds_since(uint64_t start) { return ticks - start; }
static void log_reboot_force_reset(void) { reset_attempts++; longjmp(reset_target, 1); }
static void log_reboot_yield(void) { ticks++; }
static void flush_dcache(uint64_t a, uint64_t b) { (void)a; (void)b; }
static void clean_dcache(uint64_t a, uint64_t b) { (void)a; (void)b; }
static void psci_call(uint64_t n, uint64_t a, uint64_t b, uint64_t c)
{ (void)n; (void)a; (void)b; (void)c; assert(0 && "unexpected legacy reset path"); }
#include "log-reboot.inc"

int main(void)
{
    char nonce[17];
    assert(log_reboot_parse_nonce("x=1 ace5_run_nonce=a21c4a5f00b81234 end", nonce));
    assert(!strcmp(nonce, "a21c4a5f00b81234"));
    assert(!log_reboot_parse_nonce("ace5_run_nonce=a21", nonce));
    assert(!log_reboot_parse_nonce("ace5_run_nonce=a21c4a5f00b812345", nonce));
    assert(!log_reboot_parse_nonce("ace5_run_nonce=A21c4a5f00b81234", nonce));
    assert(!log_reboot_parse_nonce("ace5_run_nonce=0000000000000001 ace5_run_nonce=0000000000000002", nonce));
    ticks = 1;
    state = GIULIA_WDT_ENABLED;
    assert(!log_reboot_arm() && !writes && !disables);
    state = 0;
    assert(log_reboot_arm() && log_reboot_owned && state == GIULIA_WDT_ENABLED);
    assert(bark == 55000 && bite == 60000);
    ticks = 44;
    log_reboot_check_startup(0);
    ticks++;
    if (!setjmp(reset_target)) { log_reboot_check_startup(0); assert(0); }
    other_cpu_done = 1;
    quiet_reset_on_request((uint64_t)(uintptr_t)&firmware_quiet);
    assert(firmware_quiet.reset_ack == LOADER_QUIET_RESET_ARMED);
    assert(bark == GIULIA_WDT_BARK_MS && bite == GIULIA_WDT_BITE_MS);
    assert(state == GIULIA_WDT_ENABLED && !disables && pets == 4);
    ticks = 100;
    log_reboot_check_startup(0);
    other_cpu_done = 0;
    if (!setjmp(reset_target)) { quiet_reset_on_request((uint64_t)(uintptr_t)&firmware_quiet); assert(0); }
    failed_id = GIULIA_WDT_PET;
    if (!setjmp(reset_target)) { log_reboot_retime(1); assert(0); }
    assert(reset_attempts == 3 && state == GIULIA_WDT_ENABLED && !disables);
    puts("PASS: pre-handoff watchdog, CPU1 return ownership, timeouts, and run nonce");
    return 0;
}
