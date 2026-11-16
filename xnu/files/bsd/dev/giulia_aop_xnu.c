/* SPDX-License-Identifier: BSD-2-Clause */
/* Explicit, finite Ace 5 AOP channel handshake (mode 1), stock WLAN PDC
 * table delivery (mode 2), or current-firmware CX diagnostic (mode 3).
 * None enables the WLAN radio or RPMh rails.
 * Loader quiet ACK pauses its firmware clients and masks its CPU interrupts;
 * single-core XNU and the board guards exclude the old Redwood QMP clients.
 */
#ifndef GIULIA_AOP_XNU_HOST_TEST
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <sys/hackios_platform.h>
#include <sys/loader_firmware_quiet.h>
#include <sys/systm.h>
#include <vm/memory_types.h>
#endif
#include "giulia_aop.h"
#include "giulia_wlan_pdc.h"
#include "giulia_wifi_gpio.h"
#include "giulia_cx_vote.h"
#include "giulia_rpmh_probe.h"
extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, unsigned int);
extern void ml_io_unmap(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);
extern void loader_log_line(const char *);
struct ga_mapping { uintptr_t allocation, address; vm_size_t size; };
struct ga_trial {
    struct ga_mapping shared, msg, ipcc, gcc, tlmm, stats, db;
    volatile struct loader_firmware_quiet *quiet;
    struct giulia_aop aop;
    struct giulia_wlan_pdc pdc;
    struct giulia_wifi_gpio gpio;
    uint8_t table[508];
    uint32_t guard_failed, guard_pa, guard_mask, guard_expected, guard_observed;
};
static struct ga_trial ga;
static unsigned ga_started, ga_pdc_mode, ga_cx_mode;
static void ga_log(const char *line) { loader_log_line(line); printf("%s", line); }
static void ga_barrier(__unused void *cookie)
{
#ifndef GIULIA_AOP_XNU_HOST_TEST
    __asm__ volatile("dsb sy" ::: "memory");
#else
    __sync_synchronize();
#endif
}
static void ga_map(struct ga_mapping *m, uint64_t pa, vm_size_t bytes, unsigned flags)
{
    vm_offset_t page = trunc_page(pa), offset = pa - page;
    vm_size_t size = round_page(offset + bytes);
    m->allocation = ml_io_map_unmappable(page, size, flags);
    if (m->allocation) { m->address = m->allocation + offset; m->size = size; }
}
static void ga_unmap(struct ga_mapping *m)
{
    if (m->allocation) ml_io_unmap(m->allocation, m->size);
    *m = (struct ga_mapping){0};
}
static uint32_t ga_mmio(struct ga_mapping *m, uint32_t offset)
{
    uint32_t v = *(volatile uint32_t *)(m->address + offset);
    ga_barrier(0); return v;
}
static int ga_check(struct ga_trial *p, uint32_t pa, uint32_t got, uint32_t mask, uint32_t expected)
{
    if (got != UINT32_MAX && (got & mask) == expected) return 1;
    if (!p->guard_failed) {
        p->guard_failed = 1; p->guard_pa = pa; p->guard_mask = mask;
        p->guard_expected = expected; p->guard_observed = got;
    }
    return 0;
}
/* This finite, single-CPU entry is the sole local WLAN/BT/GCC client while
 * the real loader quiet lease is held. Admit only a collapsed PCIe host and
 * inactive controls; a table/channel ACK is not used as a quiescence proof.
 * Pin/clock reads are rechecked before every AOP write and doorbell. */
static int ga_pdc_quiet(struct ga_trial *p)
{
    static const uint32_t offsets[] = {0x6b004, 0x6b008, 0x6c000,
        0x6c004, 0x5214c, 0x52008};
    static const uint32_t masks[] = {7, 0x18000, 7, 0x18000, 9, 0x3f};
    static const uint32_t expected[] = {1, 0x8000, 1, 0x8000, 9, 0};
    uint32_t q = (p->quiet->request == 1) | ((p->quiet->ack == 1) << 1) |
        ((p->quiet->active == 1) << 2);
    if (!ga_check(p, 0, q, 7, 7)) return 0;
    for (unsigned i = 0; i < 6; i++) {
        if (!ga_check(p, 0x100000u + offsets[i], ga_mmio(&p->gcc, offsets[i]), masks[i], expected[i])) return 0;
    }
    for (unsigned pin = 16; pin <= 19; pin++) {
        uint32_t mask = (15u << 2) | (1u << 12), want = 0;
        uint32_t cfg = ga_mmio(&p->tlmm, pin * 0x1000u);
        uint32_t data = ga_mmio(&p->tlmm, pin * 0x1000u + 4);
        uint32_t irq = ga_mmio(&p->tlmm, pin * 0x1000u + 8);
        if (pin <= 17 && p->gpio.bt_owned) { mask |= 1u << 9; want = 1u << 9; }
        /* The radio drives SW_CTRL/WLAN_SW_CTRL; never drive these inputs. */
        if (pin >= 18) mask |= 1u << 9;
        uint32_t pa = GIULIA_WIFI_GPIO_PA + pin * 0x1000u;
        if (!ga_check(p, pa, cfg, mask, want) || !ga_check(p, pa + 4, data, 3, 0) ||
            !ga_check(p, pa + 8, irq, 1, 0)) return 0;
    }
    return 1;
}
static uint32_t ga_gpio_read(void *cookie, uint32_t offset)
{ return ga_mmio(&((struct ga_trial *)cookie)->tlmm, offset); }
static void ga_gpio_write(void *cookie, uint32_t offset, uint32_t value)
{ *(volatile uint32_t *)(((struct ga_trial *)cookie)->tlmm.address + offset) = value; }
static uint32_t ga_read(void *cookie, uint32_t offset)
{
    struct ga_trial *p = cookie;
    uint32_t v = *(volatile uint32_t *)(p->msg.address + offset);
    ga_barrier(0); return v;
}
static int ga_cx_read(void *cookie,uint32_t offset,uint32_t *value)
{ *value=ga_mmio(&((struct ga_trial *)cookie)->stats,offset); return 0; }
static int ga_cx_quiet(struct ga_trial *p)
{
    return ga_check(p,0,(p->quiet->request==1) | ((p->quiet->ack==1)<<1) |
        ((p->quiet->active==1)<<2),7,7);
}
static void ga_write(void *cookie, uint32_t offset, uint32_t value)
{
    struct ga_trial *p = cookie;
    if (ga_pdc_mode && !ga_pdc_quiet(p)) return;
    if (ga_cx_mode && !ga_cx_quiet(p)) return;
    *(volatile uint32_t *)(p->msg.address + offset) = value;
    ga_barrier(0);
}
static void ga_delay(__unused void *cookie, unsigned us) { delay(us); }
static int ga_kick(void *cookie)
{
    struct ga_trial *p = cookie;
    if (ga_pdc_mode && !ga_pdc_quiet(p)) return GIULIA_AOP_PREREQUISITE;
    if (ga_cx_mode && !ga_cx_quiet(p)) return GIULIA_AOP_PREREQUISITE;
    /* Fixed official qcom-ipcc.c: SEND_ID=0xc, client AOP=0, signal QMP=0.
     * Receive routing is left with firmware; ACKs are polled in message RAM. */
    *(volatile uint32_t *)(p->ipcc.address + 0xcu) = 0;
    ga_barrier(0); return 0;
}
static int ga_quiet_acquire(struct ga_trial *p)
{
    p->quiet->request = 1; ga_barrier(0);
    for (unsigned i = 0; i < 10000; i++) {
        if (p->quiet->ack == 1 && p->quiet->active == 1) return 1;
        ga_delay(0, 100);
    }
    p->quiet->request = 0; ga_barrier(0);
    return 0;
}
static int ga_quiet_release(struct ga_trial *p)
{
    p->quiet->request = 0; ga_barrier(0);
    for (unsigned i = 0; i < 10000; i++) {
        if (!p->quiet->ack && !p->quiet->active) return 1;
        ga_delay(0, 100);
    }
    return 0;
}
static void ga_thread(void *context, __unused wait_result_t wait)
{
    uint64_t pa = (uint64_t)(uintptr_t)context;
    struct giulia_aop_snapshot before, after;
    struct giulia_arc_resource cx={0};
    struct giulia_aop_io io = {&ga, ga_read, ga_write, ga_barrier, ga_delay, ga_kick};
    int opened = 0, closed = 0, released = 0, rc;
    ga_map(&ga.shared, pa, sizeof(struct loader_firmware_quiet), VM_WIMG_WCOMB);
    if (!ga.shared.address) goto done;
    ga.quiet = (volatile struct loader_firmware_quiet *)ga.shared.address;
    if (ga.quiet->magic != LOADER_QUIET_MAGIC || ga.quiet->version != LOADER_QUIET_VERSION ||
        ga.quiet->size != sizeof(*ga.quiet) || ga.quiet->request || ga.quiet->ack || ga.quiet->active) {
        ga_log("ace5-aop: invalid/busy loader quiet service; no device writes\n"); goto done;
    }
    ga_map(&ga.msg, GIULIA_AOP_MSGRAM_PHYS, GIULIA_AOP_MSGRAM_SIZE, VM_WIMG_IO);
    ga_map(&ga.ipcc, 0x00406000u, 0x1000u, VM_WIMG_IO);
    if (!ga.msg.address || !ga.ipcc.address) goto done;
    if (ga_cx_mode) {
        ga_map(&ga.stats,GIULIA_CX_STATS_PHYS,GIULIA_CX_STATS_SIZE,VM_WIMG_IO);
        ga_map(&ga.db,0x81c60000u,GIULIA_RPMH_DB_MAX,VM_WIMG_WCOMB);
        if (!ga.stats.address || !ga.db.address) goto done;
        uint32_t version=*(volatile uint32_t *)ga.db.address;
        if (giulia_arc_resolve((const uint8_t *)ga.db.address,GIULIA_RPMH_DB_MAX,
            version,"cx.lvl",&cx) || cx.address!=0x30000u) {
            ga_log("ace5-cx-vote: current CX CMD DB profile rejected; no query sent\n"); goto done;
        }
    }
    if (ga_pdc_mode) {
        ga_map(&ga.gcc, 0x00100000u, 0x70000u, VM_WIMG_IO);
        ga_map(&ga.tlmm, GIULIA_WIFI_GPIO_PA, GIULIA_WIFI_GPIO_SIZE, VM_WIMG_IO);
        if (!ga.gcc.address || !ga.tlmm.address) goto done;
    }
    if (!ga_quiet_acquire(&ga)) {
        ga_log("ace5-aop: loader quiet ACK timeout; no device writes\n"); goto done;
    }
    if (ga_pdc_mode) {
        struct giulia_wifi_gpio_io gpio_io = {&ga, ga_gpio_read, ga_gpio_write,
            ga_barrier, ga_delay};
        if (!ga_pdc_quiet(&ga)) {
            ga_log("ace5-pdc: host/BT/control lines are not quiet; no power message sent\n"); goto release;
        }
        rc = giulia_wifi_gpio_begin_with_bt_reset(&ga.gpio,
            GIULIA_WGPIO_BEGIN_REQUIRED | GIULIA_WGPIO_BT_CONTROL_EXCLUSIVE, &gpio_io);
        if (rc || !ga_pdc_quiet(&ga)) {
            ga_log("ace5-pdc: GPIO isolation incomplete; no power message sent\n"); goto cleanup;
        }
    }
    /* With firmware parked, this single XNU CPU has the only local QMP
     * executor; the core also holds its process-wide QMP owner token. */
    rc = giulia_aop_inspect(&io, GIULIA_AOP_MSGRAM_SIZE, &before);
    if (rc || before.changed || before.message_length) goto cleanup;
    rc = giulia_aop_open(&ga.aop, &io, GIULIA_AOP_MSGRAM_SIZE,
        GIULIA_AOP_REQUIRED | GIULIA_AOP_INHERITED_CHANNEL_LEASE,
        GIULIA_AOP_OPEN_ON_INHERITED_LINK);
    opened = rc == 0;
    if (opened) ga_log("ace5-aop: channel open acknowledged; no power message sent\n");
    if (opened && ga_cx_mode) {
        struct giulia_cx_vote s;
        char status[192];
        const struct giulia_cx_vote_io ci={&ga,ga_cx_read,GIULIA_CX_STATS_PHYS,GIULIA_CX_STATS_SIZE};
        rc=giulia_cx_vote_capture(&ga.aop,&ci,&cx,&s);
        snprintf(status,sizeof(status),"ace5-cx-vote: rc=%d valid=%u reads=%u pending=%u callback=%d\n",
            rc,s.valid,s.reads,ga.aop.pending,s.callback_error); ga_log(status);
        for (unsigned i=0;i<7;i++) {
            snprintf(status,sizeof(status),"ace5-cx-vote: word%u before=%#x after=%#x\n",i,s.words[i],s.after[i]);
            ga_log(status);
        }
        if (!rc) {
            snprintf(status,sizeof(status),"ace5-cx-vote: HLOS selector=%u corner=%u; observation only\n",
                s.selector[2],cx.level[s.selector[2]]); ga_log(status);
            ga_log("ace5-cx-vote: AOP capture complete; no ARC access or power requests\n");
        }
    }
    if (opened && ga_pdc_mode) {
        char status[192];
        rc = giulia_wlan_pdc_start(&ga.pdc, &ga.aop, ga.table, sizeof(ga.table),
            NULL, GIULIA_WLAN_PDC_REQUIRED, GIULIA_WLAN_PDC_KEEP_TABLE);
        snprintf(status, sizeof(status), "ace5-pdc: rc=%d attempted=0x%x delivered=0x%x pending=%u\n",
            rc, ga.pdc.attempted, ga.pdc.delivered, ga.aop.pending);
        ga_log(status);
        if (ga.pdc.attempted) {
            if (!rc && ga.pdc.delivered == GIULIA_WLAN_PDC_ALL && ga_pdc_quiet(&ga) &&
                giulia_wlan_pdc_stop(&ga.pdc, GIULIA_WLAN_PDC_REQUIRED) == GIULIA_WLAN_PDC_RETAINED)
                ga_log("ace5-pdc: all 10 stock messages acknowledged; table/GPIO/firmware quiet retained until kernel return\n");
            else ga_log("ace5-pdc: partial/uncertain delivery; no resend; retain until cold reboot\n");
            ga_log("ace5-pdc: QMP ACK is not PMIC readback; WLAN and rail startup NOT attempted\n");
            goto retain;
        }
        if (ga.pdc.state != GIULIA_WLAN_PDC_IDLE &&
            giulia_wlan_pdc_stop(&ga.pdc, GIULIA_WLAN_PDC_REQUIRED)) goto retain;
    }
cleanup:
    if (ga.aop.owned) {
        rc = giulia_aop_close(&ga.aop);
        if (rc) {
            ga_log("ace5-aop: channel cleanup failed; firmware remains parked until kernel return\n");
            goto retain;
        }
    }
    if (opened && giulia_aop_inspect(&io, GIULIA_AOP_MSGRAM_SIZE, &after) == 0 && !after.changed &&
        after.words[0x24/4] == before.words[0x24/4] &&
        after.words[0x28/4] == before.words[0x28/4] &&
        after.words[0x2c/4] == before.words[0x2c/4] &&
        after.words[0x30/4] == before.words[0x30/4] && !after.message_length)
        closed = 1;
    if (ga_pdc_mode && ga.gpio.state != GIULIA_WGPIO_IDLE &&
        giulia_wifi_gpio_stop(&ga.gpio, GIULIA_WGPIO_STOP_REQUIRED | GIULIA_WGPIO_BT_CONTROL_EXCLUSIVE)) {
        ga_log("ace5-pdc: GPIO cleanup incomplete; retain until cold reboot\n"); goto retain;
    }
release:
    released = ga_quiet_release(&ga);
    if (!ga_pdc_mode && opened && closed && released)
        ga_log("ace5-aop: channel handshake complete; inherited link preserved; firmware resumed\n");
    else ga_log("ace5-aop: channel handshake incomplete; preserve diagnostics\n");
    if (!released) goto retain;
done:
    ga_unmap(&ga.db); ga_unmap(&ga.stats);
    ga_unmap(&ga.tlmm); ga_unmap(&ga.gcc);
    ga_unmap(&ga.ipcc); ga_unmap(&ga.msg); ga_unmap(&ga.shared);
retain:
    if (ga.guard_failed) {
        char status[192];
        snprintf(status, sizeof(status), "ace5-pdc: first guard failure pa=0x%x mask=0x%x expected=0x%x observed=0x%x\n",
            ga.guard_pa, ga.guard_mask, ga.guard_expected, ga.guard_observed);
        ga_log(status);
    }
    thread_terminate(current_thread());
}
void giulia_aop_test_init(void)
{
    uint32_t enabled = 0, cpus = 0, cpu = 0, seconds = 0;
    DTEntry entry; const void *property; unsigned size;
    uint64_t pa;
    thread_t thread;
    if (!PE_parse_boot_argn("ace5_aop_channel_test", &enabled, sizeof(enabled)) ||
        (enabled != 1 && enabled != 2 && enabled != 3 && enabled != 4)) return;
    if (hackios_platform() != HACKIOS_PLATFORM_GIULIA) return;
    if (__atomic_load_n(&ga_started, __ATOMIC_ACQUIRE)) return;
    PE_parse_boot_argn("cpus", &cpus, sizeof(cpus));
    PE_parse_boot_argn("bootcpu", &cpu, sizeof(cpu));
    PE_parse_boot_argn("loader_return_after", &seconds, sizeof(seconds));
    if (cpus != 1 || cpu != 1 || ml_get_topology_info()->num_cpus != 1 || seconds < 10 || seconds > 300) {
        ga_log("ace5-aop: requires cpus=1 bootcpu=1 and a finite 10..300 second run\n"); return;
    }
    const char *const incompatible[] = {"ace5_wifi_probe", "ace5_touch", "ace5_battery", "ace5_cpu_khz", "ace5_gpu_probe", "ace5_crm_channel_test"};
    for (unsigned i = 0; i < sizeof(incompatible)/sizeof(incompatible[0]); i++) {
        uint32_t value = 0;
        if (PE_parse_boot_argn(incompatible[i], &value, sizeof(value)) && value) {
            ga_log("ace5-aop: another hardware probe is enabled; no test started\n"); return;
        }
    }
    if (SecureDTLookupEntry(NULL, "/chosen", &entry) != kSuccess ||
        SecureDTGetProperty(entry, "loader-firmware-quiet", &property, &size) != kSuccess || size != sizeof(pa)) {
        ga_log("ace5-aop: loader has no firmware quiet service; no device writes\n"); return;
    }
    __builtin_memcpy(&pa, property, sizeof(pa));
    if (!pa || (pa & 127u)) return;
    if (enabled == 4 && seconds > 120) return;
    if (enabled == 2) {
        if (seconds > 120 || SecureDTGetProperty(entry, "loader-wlan-pdc-table", &property, &size) != kSuccess ||
            size != sizeof(ga.table)) {
            ga_log("ace5-pdc: requires current DT table and finite run <=120 seconds; no device writes\n"); return;
        }
        __builtin_memcpy(ga.table, property, sizeof(ga.table));
    }
    if (__atomic_exchange_n(&ga_started, 1, __ATOMIC_ACQ_REL)) return;
    if (enabled == 4) { giulia_rpmh_read_test_start(pa); return; }
    ga_pdc_mode = enabled == 2;
    ga_cx_mode = enabled == 3;
    if (kernel_thread_start(ga_thread, (void *)(uintptr_t)pa, &thread) == KERN_SUCCESS)
        thread_deallocate(thread);
    else ga_log("ace5-aop: no test thread\n");
}
