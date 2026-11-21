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
static unsigned persisted, refusals, safe_resets, failure_at, ncalls, reject_status;
static uint64_t late_ctl;
static unsigned reject_recheck;
#define DIRTY_HIGH UINT64_C(0xa5a5a5a500000000)
static int storage_ok = 1;
static char line[96];
static void print(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (!strcmp(fmt,"log-reboot: pre-arm refused id=%s step=%s detail=0x%x\n")) {
        const char *tag=va_arg(ap,const char *);
        const char *stage=va_arg(ap,const char *);
        uint64_t value=va_arg(ap,uint64_t); /* loader's custom %x consumes uint64_t */
        assert(value<=UINT32_MAX);
        snprintf(line,sizeof(line),"log-reboot: pre-arm refused id=%s step=%s detail=0x%llx\n",
            tag,stage,(unsigned long long)value);
    } else {
        vsnprintf(line, sizeof(line), fmt, ap);
    }
    va_end(ap);
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
    assert(ncalls < sizeof(calls) / sizeof(calls[0]));
    calls[ncalls++] = (struct op){id,a1,a2};
    if (id == failed_id || failure_at == ncalls) return DIRTY_HIGH | UINT32_MAX;
    if (id == GIULIA_WDT_STATUS) {
        int recheck=(failure_at==2 || failure_at==3) && ncalls==failure_at+1;
        if (recheck && reject_recheck) return DIRTY_HIGH | UINT32_C(0x80000000);
        if (recheck) state=late_ctl;
        if (r1) *r1=DIRTY_HIGH | ((reject_status && ncalls==6) ? 0 : state);
        if (r2) *r2=DIRTY_HIGH | UINT32_C(0x98765432);
        return DIRTY_HIGH;
    }
    writes++;
    if (id == GIULIA_WDT_SET_TIME) {
        if (a1 != GIULIA_WDT_NO_CHANGE) bark = a1;
        if (a2 != GIULIA_WDT_NO_CHANGE) bite = a2;
        if (state & GIULIA_WDT_ENABLED) assert(bark < bite);
    } else if (id == GIULIA_WDT_CONTROL) {
        state = a1 == 3 ? GIULIA_WDT_ENABLED : 0;
    } else { assert(id == GIULIA_WDT_PET); pets++; }
    return DIRTY_HIGH;
}
static int write_logs(const char *ignored, uint64_t size)
{
    assert(!ignored && !size);
    if (strstr(line,"pre-arm refused id=a21c4a5f00b81234")) {
        assert(!state && !log_reboot_owned && !reject_recheck);
        assert(calls[ncalls-1].id==GIULIA_WDT_STATUS);
        for (unsigned i=0;i<ncalls;i++) assert(calls[i].id!=GIULIA_WDT_CONTROL);
        assert(strstr(line," step=SET_BITE ") || strstr(line," step=SET_BARK "));
        assert(strstr(line," detail=0xffffffff\n"));
        refusals++;
    } else {
        assert(strstr(line,"pre-arm verified id=a21c4a5f00b81234") && log_reboot_owned);
        persisted++;
    }
    return storage_ok;
}
static int quiet_watchdog_disarm(void) { disables++; state = 0; return 0; }
static uint64_t seconds_since(uint64_t start) { return ticks - start; }
static void log_reboot_force_reset(void) { reset_attempts++; longjmp(reset_target, 1); }
static void log_reboot_yield(void) { ticks++; }
static void flush_dcache(uint64_t a, uint64_t b) { (void)a; (void)b; }
static void clean_dcache(uint64_t a, uint64_t b) { (void)a; (void)b; }
static void psci_call(uint64_t n, uint64_t a, uint64_t b, uint64_t c)
{ assert(n == PSCI_SYSTEM_RESET && !a && !b && !c && !log_reboot_owned); safe_resets++; }
#include "watchdog-smc32.inc"
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
    strcpy(nonce,"a21c4a5f00b81234");
    uint64_t ctl = 0, elapsed = 0;
    assert(!giulia_wdt_smc32(GIULIA_WDT_STATUS,0,0,&ctl,&elapsed));
    assert(!ctl && elapsed==UINT32_C(0x98765432));
    ncalls=0; ticks=0;
    assert(!log_reboot_preflight(nonce) && !ncalls && !persisted && !refusals);
    assert(strstr(line," step=COUNTER detail=0x0\n"));
    ticks=1; state = GIULIA_WDT_ENABLED;
    assert(!log_reboot_preflight(nonce) && !writes && !disables && safe_resets == 2 && !refusals);
    assert(strstr(line," step=IDLE_CTL detail=0x1\n"));
    state = GIULIA_WDT_EXPIRED; ncalls = 0;
    assert(!log_reboot_preflight(nonce) && ncalls == 1 && !disables && !persisted && !refusals);
    assert(strstr(line," step=IDLE_CTL detail=0x80000000\n"));
    static const char *failed_phase[]={"","IDLE_STATUS","SET_BITE","SET_BARK","ENABLE","PET","STATUS"};
    for (failure_at = 1; failure_at <= 6; failure_at++) {
        ncalls = 0; state = 0; log_reboot_owned = 0;
        if (!setjmp(reset_target)) {
            assert(!log_reboot_preflight(nonce) && failure_at <= 4);
        } else {
            assert(failure_at >= 5 && state == GIULIA_WDT_ENABLED && log_reboot_owned);
        }
        unsigned extra=(failure_at==2 || failure_at==3);
        assert(ncalls == failure_at + extra && !persisted && !disables);
        assert(refusals==(failure_at<2 ? 0u : (failure_at<3 ? 1u : 2u)));
        if (extra) assert(calls[ncalls-1].id==GIULIA_WDT_STATUS);
        char expected[96];
        snprintf(expected,sizeof(expected),"log-reboot: pre-arm refused id=%s step=%s detail=0xffffffff\n",
            nonce,failed_phase[failure_at]);
        assert(!strcmp(line,expected));
        for (unsigned i=0; i<failure_at; i++) {
            assert(calls[i].id == boot_ops[i].id && calls[i].a == boot_ops[i].a && calls[i].b == boot_ops[i].b);
        }
    }
    /* A failed SET may only write on a fresh, positive idle STATUS.
     * An unreadable status, someone else's enable or an expiry must not write. */
    failure_at=3; log_reboot_owned=0;
    for (unsigned i=0;i<3;i++) {
        state=0; ncalls=0;
        reject_recheck=i==0;
        late_ctl=i==1 ? GIULIA_WDT_ENABLED : GIULIA_WDT_EXPIRED;
        assert(!log_reboot_preflight(nonce) && ncalls==4);
        assert(refusals==2 && !persisted && !disables);
    }
    /* A failed attempt to persist the confirmed-idle refusal still resets. */
    late_ctl=0; reject_recheck=0; ncalls=0; state=0; storage_ok=0;
    unsigned resets_before=safe_resets;
    assert(!log_reboot_preflight(nonce) && refusals==3 && !persisted && safe_resets==resets_before+1);
    storage_ok=1;
    failure_at=0; ncalls=0; state=0; log_reboot_owned=0; reject_status=1;
    if (!setjmp(reset_target)) { log_reboot_preflight(nonce); assert(0); }
    assert(ncalls==6 && log_reboot_owned && !persisted && !disables && refusals==3);
    assert(strstr(line," step=STATUS_CTL detail=0x0\n"));
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
    puts("PASS: Giulia SMC32 width, strict refusal phases and reconfirmed idle-only refusal persistence");
    puts("PASS: long deadlines before enable, verified pre-arm checkpoint and failure ownership");
    puts("PASS: pre-handoff watchdog, CPU1 return ownership, timeouts, and run nonce");
    return 0;
}
