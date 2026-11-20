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
#include <stdarg.h>
static unsigned persisted, safe_resets, failure_at, ncalls, reject_status;
static int storage_ok = 1;
static char line[96];
static void print(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
}
static int write_logs(const char *ignored, uint64_t size)
{
    assert(!ignored && !size && strstr(line,"pre-arm verified id=a21c4a5f00b81234"));
    persisted++;
    return storage_ok;
}
struct op { uint64_t id, a, b; };
static struct op calls[64];
static struct loader_firmware_quiet firmware_quiet;
static int log_reboot_owned;
static uint64_t state, ticks, bark, bite, other_cpu_done;
static unsigned writes, pets, disables, reset_attempts;
static uint64_t failed_id;
static jmp_buf reset_target;

static uint64_t smc_call(uint64_t id, uint64_t a1, uint64_t a2, uint64_t *r1, uint64_t *r2)
{
    (void)r2;
    assert(ncalls < sizeof(calls) / sizeof(calls[0]));
    calls[ncalls++] = (struct op){id,a1,a2};
    if (id == failed_id || failure_at == ncalls) return 1;
    if (reject_status && ncalls == 6) { *r1 = 0; return 0; }
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
{ assert(n == PSCI_SYSTEM_RESET && !a && !b && !c && !log_reboot_owned); safe_resets++; }
#include "log-reboot.inc"

int main(void)
{
    char nonce[17];
#define ARGS "cpus=1 bootcpu=1 ace5_wifi_bringup=1 ace5_wifi_bringup_stage=4 ace5_wifi_sm_diag=5 ace5_wifi_cold_reset=1 ace5_wifi_log_reboot=1"
    assert(log_reboot_args_unambiguous(ARGS " loader_return_after=60", 60));
    assert(log_reboot_args_unambiguous(ARGS " loader_return_after=120", 120));
    assert(!log_reboot_args_unambiguous(ARGS " loader_return_after=120xxx", 120));
    assert(!log_reboot_args_unambiguous(ARGS " loader_return_after=60", 120));
    assert(!log_reboot_args_unambiguous(ARGS " loader_return_after=75 cpus=4", 75));
    assert(!log_reboot_args_unambiguous(ARGS " loader_return_after=75 ace5_wifi_sm_diag=7", 75));
    assert(!log_reboot_args_unambiguous(ARGS " loader_return_after=75 cpumask=0xf", 75));
    assert(!log_reboot_args_unambiguous(ARGS " loader_return_after=75 hv_wdt_pet=1", 75));
    assert(!log_reboot_args_unambiguous(ARGS " loader_return_after=75 hv_wdt_pet", 75));
    assert(!log_reboot_args_unambiguous(ARGS " loader_return_after=75	foo=1", 75));
    assert(!log_reboot_args_unambiguous(ARGS, 75));
    assert(log_reboot_parse_nonce("x=1 ace5_run_nonce=a21c4a5f00b81234 end", nonce));
    assert(!strcmp(nonce, "a21c4a5f00b81234"));
    assert(!log_reboot_parse_nonce("ace5_run_nonce=a21", nonce));
    assert(!log_reboot_parse_nonce("ace5_run_nonce=a21c4a5f00b812345", nonce));
    assert(!log_reboot_parse_nonce("ace5_run_nonce=A21c4a5f00b81234", nonce));
    assert(!log_reboot_parse_nonce("ace5_run_nonce=0000000000000001 ace5_run_nonce=0000000000000002", nonce));
    static const struct op boot_ops[] = {
        {GIULIA_WDT_STATUS, 0, 0},
        {GIULIA_WDT_SET_TIME, GIULIA_WDT_NO_CHANGE, 60000},
        {GIULIA_WDT_SET_TIME, 55000, GIULIA_WDT_NO_CHANGE},
        {GIULIA_WDT_CONTROL, 3, 0}, {GIULIA_WDT_PET, 0, 0},
        {GIULIA_WDT_STATUS, 0, 0}
    };
    ticks = 1;
    state = GIULIA_WDT_ENABLED;
    assert(!log_reboot_preflight(nonce) && !writes && !disables && safe_resets == 1 && !persisted);
    state = GIULIA_WDT_EXPIRED; ncalls = 0;
    assert(!log_reboot_preflight(nonce) && ncalls == 1 && !disables && !persisted);
    for (failure_at = 1; failure_at <= 6; failure_at++) {
        ncalls = 0; state = 0; log_reboot_owned = 0;
        if (!setjmp(reset_target)) {
            assert(!log_reboot_preflight(nonce) && failure_at <= 4);
        } else {
            assert(failure_at >= 5 && state == GIULIA_WDT_ENABLED && log_reboot_owned);
        }
        assert(ncalls == failure_at && !persisted && !disables);
        for (unsigned i=0; i<ncalls; i++) {
            assert(calls[i].id == boot_ops[i].id && calls[i].a == boot_ops[i].a && calls[i].b == boot_ops[i].b);
        }
    }
    failure_at=0; ncalls=0; state=0; log_reboot_owned=0; reject_status=1;
    if (!setjmp(reset_target)) { log_reboot_preflight(nonce); assert(0); }
    assert(ncalls==6 && log_reboot_owned && !persisted && !disables);
    reject_status=0; ncalls=0; state=0; log_reboot_owned=0; storage_ok=0;
    strcpy(nonce,"a21c4a5f00b81234");
    if (!setjmp(reset_target)) { log_reboot_preflight(nonce); assert(0); }
    assert(persisted==1 && log_reboot_owned && state==GIULIA_WDT_ENABLED && !disables);
    ncalls=0; state=0; log_reboot_owned=0; storage_ok=1;
    reset_attempts = pets = 0;
    assert(log_reboot_preflight(nonce) && log_reboot_owned && state == GIULIA_WDT_ENABLED);
    assert(ncalls==6 && bark == 55000 && bite == 60000 && persisted==2);
    ticks = 44;
    log_reboot_check_startup(0);
    ticks++;
    if (!setjmp(reset_target)) { log_reboot_check_startup(0); assert(0); }
    other_cpu_done = 1;
    quiet_reset_on_request((uint64_t)(uintptr_t)&firmware_quiet);
    assert(firmware_quiet.reset_ack == LOADER_QUIET_RESET_ARMED);
    assert(bark == GIULIA_WDT_BARK_MS && bite == GIULIA_WDT_BITE_MS);
    assert(calls[6].id==GIULIA_WDT_SET_TIME && calls[6].a==GIULIA_WDT_BARK_MS && calls[6].b==GIULIA_WDT_NO_CHANGE);
    assert(calls[7].id==GIULIA_WDT_SET_TIME && calls[7].a==GIULIA_WDT_NO_CHANGE && calls[7].b==GIULIA_WDT_BITE_MS);
    assert(ncalls==11 && calls[8].id==GIULIA_WDT_PET && calls[9].id==GIULIA_WDT_STATUS && calls[10].id==GIULIA_WDT_PET);
    assert(state == GIULIA_WDT_ENABLED && !disables && pets == 3);
    ticks = 100;
    log_reboot_check_startup(0);
    other_cpu_done = 0;
    if (!setjmp(reset_target)) { quiet_reset_on_request((uint64_t)(uintptr_t)&firmware_quiet); assert(0); }
    failed_id = GIULIA_WDT_PET;
    if (!setjmp(reset_target)) { log_reboot_retime(); assert(0); }
    assert(reset_attempts == 3 && state == GIULIA_WDT_ENABLED && !disables && persisted==2);
    puts("PASS: long deadlines before enable, verified pre-arm checkpoint and failure ownership");
    puts("PASS: pre-handoff watchdog, CPU1 return ownership, timeouts, and run nonce");
    return 0;
}
