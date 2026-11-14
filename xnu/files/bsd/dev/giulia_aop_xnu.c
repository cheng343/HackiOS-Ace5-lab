/* SPDX-License-Identifier: BSD-2-Clause */
/* Explicit, finite Ace 5 AOP channel handshake. No power/PDC message is sent.
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
extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, unsigned int);
extern void ml_io_unmap(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);
extern void loader_log_line(const char *);
struct ga_mapping { uintptr_t allocation, address; vm_size_t size; };
struct ga_trial {
    struct ga_mapping shared, msg, ipcc;
    volatile struct loader_firmware_quiet *quiet;
    struct giulia_aop aop;
};
static struct ga_trial ga;
static unsigned ga_started;
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
static uint32_t ga_read(void *cookie, uint32_t offset)
{
    struct ga_trial *p = cookie;
    uint32_t v = *(volatile uint32_t *)(p->msg.address + offset);
    ga_barrier(0); return v;
}
static void ga_write(void *cookie, uint32_t offset, uint32_t value)
{
    struct ga_trial *p = cookie;
    *(volatile uint32_t *)(p->msg.address + offset) = value;
    ga_barrier(0);
}
static void ga_delay(__unused void *cookie, unsigned us) { delay(us); }
static int ga_kick(void *cookie)
{
    struct ga_trial *p = cookie;
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
    if (!ga_quiet_acquire(&ga)) {
        ga_log("ace5-aop: loader quiet ACK timeout; no device writes\n"); goto done;
    }
    /* With firmware parked, this single XNU CPU has the only local QMP
     * executor; the core also holds its process-wide QMP owner token. */
    rc = giulia_aop_inspect(&io, GIULIA_AOP_MSGRAM_SIZE, &before);
    if (rc || before.changed || before.message_length) goto release;
    rc = giulia_aop_open(&ga.aop, &io, GIULIA_AOP_MSGRAM_SIZE,
        GIULIA_AOP_REQUIRED | GIULIA_AOP_INHERITED_CHANNEL_LEASE,
        GIULIA_AOP_OPEN_ON_INHERITED_LINK);
    opened = rc == 0;
    if (opened) ga_log("ace5-aop: channel open acknowledged; no power message sent\n");
    if (ga.aop.owned) {
        rc = giulia_aop_close(&ga.aop);
        if (rc) {
            ga_log("ace5-aop: channel cleanup failed; firmware remains parked until kernel return\n");
            goto retain;
        }
    }
    if (giulia_aop_inspect(&io, GIULIA_AOP_MSGRAM_SIZE, &after) == 0 && !after.changed &&
        after.words[0x24/4] == before.words[0x24/4] &&
        after.words[0x28/4] == before.words[0x28/4] &&
        after.words[0x2c/4] == before.words[0x2c/4] &&
        after.words[0x30/4] == before.words[0x30/4] && !after.message_length)
        closed = 1;
release:
    released = ga_quiet_release(&ga);
    if (opened && closed && released)
        ga_log("ace5-aop: channel handshake complete; inherited link preserved; firmware resumed\n");
    else ga_log("ace5-aop: channel handshake incomplete; preserve diagnostics\n");
    if (!released) goto retain;
done:
    ga_unmap(&ga.ipcc); ga_unmap(&ga.msg); ga_unmap(&ga.shared);
retain:
    thread_terminate(current_thread());
}
void giulia_aop_test_init(void)
{
    uint32_t enabled = 0, cpus = 0, cpu = 0, seconds = 0;
    DTEntry entry; const void *property; unsigned size;
    uint64_t pa;
    thread_t thread;
    if (!PE_parse_boot_argn("ace5_aop_channel_test", &enabled, sizeof(enabled)) || enabled != 1) return;
    if (hackios_platform() != HACKIOS_PLATFORM_GIULIA) return;
    PE_parse_boot_argn("cpus", &cpus, sizeof(cpus));
    PE_parse_boot_argn("bootcpu", &cpu, sizeof(cpu));
    PE_parse_boot_argn("loader_return_after", &seconds, sizeof(seconds));
    if (cpus != 1 || cpu != 1 || ml_get_topology_info()->num_cpus != 1 || seconds < 10 || seconds > 300) {
        ga_log("ace5-aop: requires cpus=1 bootcpu=1 and a finite 10..300 second run\n"); return;
    }
    const char *const incompatible[] = {"ace5_wifi_probe", "ace5_touch", "ace5_battery", "ace5_cpu_khz", "ace5_gpu_probe"};
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
    if (__atomic_exchange_n(&ga_started, 1, __ATOMIC_ACQ_REL)) return;
    if (kernel_thread_start(ga_thread, (void *)(uintptr_t)pa, &thread) == KERN_SUCCESS)
        thread_deallocate(thread);
    else ga_log("ace5-aop: no test thread\n");
}
