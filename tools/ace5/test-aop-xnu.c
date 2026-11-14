/* SPDX-License-Identifier: BSD-2-Clause */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../xnu/files/sys/loader_firmware_quiet.h"
#define GIULIA_AOP_XNU_HOST_TEST
#define __unused __attribute__((unused))
#define KERN_SUCCESS 0
#define kSuccess 0
#define HACKIOS_PLATFORM_GIULIA 2
#define VM_WIMG_IO 7u
#define VM_WIMG_WCOMB 6u
#define PAGE_SIZE ((uintptr_t)0x4000)
#define trunc_page(x) ((uintptr_t)(x) & ~(PAGE_SIZE-1))
#define round_page(x) (((uintptr_t)(x)+PAGE_SIZE-1)&~(PAGE_SIZE-1))
typedef uintptr_t vm_offset_t;
typedef uintptr_t vm_size_t;
typedef int kern_return_t;
typedef int wait_result_t;
typedef void *thread_t;
typedef int DTEntry;
static unsigned board, enabled, cpu, cpus, seconds, incompatible, missing_property;
static unsigned creates, exits, deallocs, maps, unmaps, fail_map, thread_error;
static unsigned no_quiet_ack, no_close_ack, complete, kicks, time_us;
static _Alignas(0x4000) uint32_t shared_page[0x4000/4], msg_page[0x4000/4], ipcc_page[0x4000/4];
static uint64_t shared_pa = 0x90000100;
static volatile struct loader_firmware_quiet *const shared =
    (volatile struct loader_firmware_quiet *)((uint8_t *)shared_page + 0x100);
static struct { unsigned num_cpus; } topology;
static void peer_barrier(void);
#define __sync_synchronize() peer_barrier()
static void delay(unsigned us) { assert(us == 100); time_us += us; assert(time_us < 5000000); }
static int hackios_platform(void) { return (int)board; }
/* Match XNU's use without including private kernel headers. */
#define ml_get_topology_info() (&topology)
static int PE_parse_boot_argn(const char *key, void *out, unsigned size)
{
    unsigned v = 0;
    assert(size == 4);
    if (!strcmp(key, "ace5_aop_channel_test")) v = enabled;
    if (!strcmp(key, "cpus")) v = cpus;
    if (!strcmp(key, "bootcpu")) v = cpu;
    if (!strcmp(key, "loader_return_after")) v = seconds;
    if (!strcmp(key, "ace5_touch")) v = incompatible;
    *(unsigned *)out = v; return v != 0;
}
static int SecureDTLookupEntry(void *root, const char *path, DTEntry *entry)
{ (void)root; assert(!strcmp(path,"/chosen")); *entry = 1; return missing_property ? -1 : 0; }
static int SecureDTGetProperty(DTEntry e, const char *key, const void **out, unsigned *size)
{ assert(e == 1 && !strcmp(key,"loader-firmware-quiet")); *out = &shared_pa; *size = 8; return 0; }
static thread_t current_thread(void) { return (void *)(uintptr_t)1; }
static void thread_deallocate(thread_t t) { assert(t == current_thread()); deallocs++; }
static int kernel_thread_start(void (*fn)(void *,wait_result_t), void *arg, thread_t *out)
{ creates++; if (thread_error) return -1; *out = current_thread(); fn(arg,0); return 0; }
static int quiet_printf(const char *fmt, ...) { (void)fmt; return 0; }
#define printf quiet_printf
#include "../../xnu/files/bsd/dev/giulia_aop_xnu.c"
#undef printf
#undef __sync_synchronize
static void peer_barrier(void)
{
    if (!no_quiet_ack) { shared->ack = shared->request; shared->active = shared->request != 0; }
    /* SEND_ID is write-only in the model: a write of target zero rings it. */
    if (ipcc_page[(0x2000+0xc)/4] == 0) {
        assert(shared->ack == 1 && shared->active == 1);
        kicks++; ipcc_page[(0x2000+0xc)/4] = 0xffffffff;
        if (msg_page[0x2c/4] == GIULIA_AOP_UP) msg_page[0x30/4] = GIULIA_AOP_UP;
        else if (!no_close_ack) msg_page[0x30/4] = GIULIA_AOP_DOWN;
    }
}
vm_offset_t ml_io_map_unmappable(vm_offset_t pa, vm_size_t size, unsigned flags)
{
    unsigned bit; void *base;
    assert(size == 0x4000);
    if (pa == 0x90000000) { bit = 1; base = shared_page; assert(flags == VM_WIMG_WCOMB); }
    else if (pa == 0xc300000) { bit = 2; base = msg_page; assert(flags == VM_WIMG_IO); }
    else { assert(pa == 0x404000 && flags == VM_WIMG_IO); bit = 4; base = ipcc_page; }
    if (bit == fail_map) return 0;
    assert(!(maps & bit)); maps |= bit; return (uintptr_t)base;
}
void ml_io_unmap(vm_offset_t address, vm_size_t size)
{
    unsigned bit = address == (uintptr_t)shared_page ? 1 : address == (uintptr_t)msg_page ? 2 : 4;
    assert(size == 0x4000 && (maps & bit) && !(unmaps & bit)); unmaps |= bit;
}
kern_return_t thread_terminate(thread_t t) { assert(t == current_thread()); exits++; return 0; }
void loader_log_line(const char *text) { if (strstr(text,"channel handshake complete;")) complete++; }
static void setup(void)
{
    memset(&ga,0,sizeof(ga)); ga_started = 0;
    memset(shared_page,0,sizeof(shared_page)); memset(msg_page,0,sizeof(msg_page));
    memset(ipcc_page,0xff,sizeof(ipcc_page));
    shared->magic = LOADER_QUIET_MAGIC; shared->version = 1; shared->size = sizeof(*shared);
    msg_page[0] = 0x4d41494c; msg_page[1] = 1;
    msg_page[0x0c/4] = msg_page[0x10/4] = msg_page[0x14/4] = msg_page[0x18/4] = GIULIA_AOP_UP;
    msg_page[0x24/4] = msg_page[0x28/4] = GIULIA_AOP_UP;
    msg_page[0x2c/4] = msg_page[0x30/4] = GIULIA_AOP_DOWN;
    msg_page[0x34/4] = 0x2d8; msg_page[0x38/4] = 0x128;
    board = 2; enabled = cpu = cpus = topology.num_cpus = 1; seconds = 90;
    incompatible = missing_property = creates = exits = deallocs = maps = unmaps = 0;
    fail_map = thread_error = no_quiet_ack = no_close_ack = complete = kicks = time_us = 0;
}
static void check_no_start(void) { giulia_aop_test_init(); assert(!creates && !maps && !kicks); }
int main(void)
{
    setup(); enabled = 0; check_no_start();
    setup(); board = 1; check_no_start();
    setup(); cpu = 0; check_no_start();
    setup(); cpus = 2; check_no_start();
    setup(); topology.num_cpus = 2; check_no_start();
    setup(); seconds = 0; check_no_start();
    setup(); seconds = 301; check_no_start();
    setup(); missing_property = 1; check_no_start();
    setup(); incompatible = 1; check_no_start();
    setup(); giulia_aop_test_init();
    assert(complete == 1 && maps == 7 && unmaps == maps && kicks == 3 && exits == 1 && deallocs == 1);
    assert(msg_page[0x24/4] == GIULIA_AOP_UP && msg_page[0x2c/4] == GIULIA_AOP_DOWN);
    assert(!shared->request && !shared->ack && !shared->active && !msg_page[0x128/4]);
    giulia_aop_test_init(); assert(creates == 1);
    for (unsigned i = 1; i <= 4; i <<= 1) {
        setup(); fail_map = i; giulia_aop_test_init();
        assert(maps == unmaps && !complete && !kicks && exits == 1);
    }
    setup(); shared->version = 2; giulia_aop_test_init(); assert(maps == unmaps && !kicks);
    setup(); no_quiet_ack = 1; giulia_aop_test_init();
    assert(!complete && !kicks && !shared->request && maps == unmaps && time_us == 1000000);
    setup(); no_close_ack = 1; giulia_aop_test_init();
    assert(!complete && !unmaps && maps == 7 && shared->request == 1 && shared->active && ga.aop.owned);
    no_close_ack = 0;
    assert(giulia_aop_close(&ga.aop) == 0 && ga_quiet_release(&ga));
    ga_unmap(&ga.ipcc); ga_unmap(&ga.msg); ga_unmap(&ga.shared);
    assert(unmaps == maps);
    puts("Ace5 AOP XNU: actual guarded MMIO adapter, quiet lease, handshake and failure retention passed");
    return 0;
}
