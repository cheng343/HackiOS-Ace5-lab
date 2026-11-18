/* SPDX-License-Identifier: BSD-2-Clause */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "loader_firmware_quiet.h"
static struct loader_firmware_quiet firmware_quiet;
static int firmware_quiet_enabled, wdt_armed, masked, mode;
static volatile uint64_t other_cpu_done;
static unsigned flushes, clean_acks, restored, pets;
static uint64_t ticks;
static int crm_reset_required, pdc_reset_required;
static int str_eq(const char *a,const char *b) { return !strcmp(a,b); }
#include "quiet-args.inc"
#define HACKIOS_GIULIA
static void select_trial(const char *cmd,int other_cpu)
{
    struct { const char *cmdline; } pi={cmd};
    unsigned kernel_mpidr=other_cpu ? 1u:0u,this_mpidr=0;
    unsigned kernel_budget=75;
    crm_reset_required=pdc_reset_required=0;
#include "quiet-enable.inc"
}
#include "crm-command-gate.inc"
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
static void quiet_reset_on_request(uint64_t base)
{
    assert(masked && firmware_quiet.active && firmware_quiet.reset_request == 1);
    firmware_quiet.reset_ack = LOADER_QUIET_RESET_ARMED;
    clean_dcache(base + 128, base + sizeof(firmware_quiet));
    other_cpu_done = 1;
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
    select_trial("cpus=1 ace5_aop_channel_test=4 bootcpu=1",1);
    assert(firmware_quiet_enabled && pdc_reset_required && !crm_reset_required);
    select_trial("ace5_aop_channel_test=4",0);
    assert(!firmware_quiet_enabled && !pdc_reset_required);
    select_trial("ace5_aop_channel_test=40",1);
    assert(!firmware_quiet_enabled && !pdc_reset_required);
    select_trial("ace5_wifi_bringup=1 ace5_wifi_bringup_stage=4 ace5_wifi_cold_reset=1 bootcpu=1",1);
    assert(firmware_quiet_enabled && firmware_quiet.reset_capability);
    select_trial("ace5_wifi_bringup=1 ace5_wifi_bringup_stage=5 ace5_wifi_cold_reset=1 bootcpu=1",1);
    assert(firmware_quiet_enabled && !firmware_quiet.reset_capability);
    puts("PASS: actual RPMh READ selection enables quiet and cold-reset gate");
    assert(quiet_flag_present("cpus=1 ace5_crm_channel_test=1 bootcpu=1","ace5_crm_channel_test=1"));
    assert(quiet_flag_present("cpus=1 ace5_crm_channel_test=2 bootcpu=1","ace5_crm_channel_test=2"));
    assert(quiet_flag_present("ace5_aop_channel_test=1","ace5_aop_channel_test=1"));
    assert(quiet_flag_present("ace5_aop_channel_test=2","ace5_aop_channel_test=2"));
    const char *invalid[]={"ace5_crm_channel_test=10","xace5_crm_channel_test=1",
        "ace5_crm_channel_test=1x","ace5_crm_channel_test=0",""};
    for (unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++)
        assert(!quiet_flag_present(invalid[i],"ace5_crm_channel_test=1"));
    crm_reset_required=pdc_reset_required=0; assert(crm_command_allowed("oem run"));
    crm_reset_required=1;
    const char *allowed[]={"getvar:result","getvar:crm-reset-required","upload",
        "oem screenshot","oem stop","reboot"};
    const char *denied[]={"oem run","oem selftest","download:1000","oem ramdisk",
        "oem touchprobe","oem live-log","oem memmap","oem clearscreen"};
    for (unsigned i=0;i<sizeof(allowed)/sizeof(allowed[0]);i++) assert(crm_command_allowed(allowed[i]));
    for (unsigned i=0;i<sizeof(denied)/sizeof(denied[0]);i++) assert(!crm_command_allowed(denied[i]));
    crm_reset_required=0; pdc_reset_required=1;
    assert(!crm_command_allowed("oem wlan-pdc-table"));
    for (unsigned i=0;i<sizeof(allowed)/sizeof(allowed[0]);i++) assert(crm_command_allowed(allowed[i]));
    for (unsigned i=0;i<sizeof(denied)/sizeof(denied[0]);i++) assert(!crm_command_allowed(denied[i]));
    puts("PASS: actual PDC retained cold-reset command gate");
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
    reset(); firmware_quiet.request = firmware_quiet.reset_capability = firmware_quiet.reset_request = 1;
    assert(!observer_firmware_quiet(0, 90, &last) && other_cpu_done && clean_acks == 3);
    assert(firmware_quiet.reset_ack == LOADER_QUIET_RESET_ARMED && !masked);
    puts("PASS: actual loader firmware quiet ACK, cancellation, kernel return and interrupt restore");
    puts("PASS: actual quiet opt-in parser and retained CRM cold-reset command gate");
    return 0;
}
