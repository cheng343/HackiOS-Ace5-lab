/* SPDX-License-Identifier: BSD-2-Clause */
/* Compile the real XNU diagnostic with host map/thread/log services. */
#include <assert.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#define GIULIA_WIFI_HOST_TEST
#define __unused __attribute__((unused))
#define __printflike(a,b)
#define KERN_SUCCESS 0
#define HACKIOS_PLATFORM_GIULIA 2
/* XNU arm64 memory_types.h and the current 16K kernel page geometry. */
#define VM_WIMG_IO 7u
#define VM_WIMG_WCOMB 6u
#define PAGE_SIZE ((uintptr_t)0x4000)
#define trunc_page(x) ((uintptr_t)(x) & ~(PAGE_SIZE - 1u))
#define round_page(x) (((uintptr_t)(x) + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u))
typedef uintptr_t vm_offset_t;
typedef uintptr_t vm_size_t;
typedef int kern_return_t;
typedef int wait_result_t;
typedef void *thread_t;
static unsigned mode, board, creates, exits, deallocs, lines, mapped, unmapped;
static uint32_t failed_pa;
static int thread_error, saw_complete, saw_noc, resolver_calls;
static _Alignas(0x4000) uint32_t gcc[0x70000 / 4], gpio[0x100000 / 4], rsc[0x10000 / 4];
static _Alignas(0x4000) uint32_t db[0x20000 / 4], sm_pages[0xc000 / 4], crm_pages[0x4000 / 4];
static uint32_t *const sm = sm_pages + 0x3000 / 4;
static uint32_t *const crm = crm_pages + 0x1000 / 4;
static uint64_t original;
static int PE_parse_boot_argn(const char *name, void *out, unsigned size)
{
    assert(!strcmp(name, "ace5_wifi_probe") && size == sizeof(unsigned));
    *(unsigned *)out = mode; return mode != 0;
}
static int hackios_platform(void) { return (int)board; }
static thread_t current_thread(void) { return (thread_t)(uintptr_t)1; }
static void thread_deallocate(thread_t t) { assert(t == current_thread()); ++deallocs; }
static kern_return_t kernel_thread_start(void (*f)(void *, wait_result_t), void *arg, thread_t *out)
{
    ++creates;
    if (thread_error) return -1;
    *out = current_thread(); f(arg, 0); return 0;
}
static int quiet_printf(const char *format, ...) { (void)format; return 0; }
#define printf quiet_printf
#include "../../xnu/files/bsd/dev/giulia_wifi.c"
#undef printf
vm_offset_t ml_io_map_unmappable(vm_offset_t pa, vm_size_t size, unsigned int flags)
{
    unsigned index; void *base; unsigned expected;
    assert(!(pa & (PAGE_SIZE - 1u)) && !(size & (PAGE_SIZE - 1u)));
    assert(flags == (pa == 0x81c60000 ? VM_WIMG_WCOMB : VM_WIMG_IO));
    switch (pa) {
    case 0x100000: index = 0; base = gcc; expected = sizeof(gcc); break;
    case 0x0f100000: index = 1; base = gpio; expected = sizeof(gpio); break;
    case 0x17a20000: index = 2; base = rsc; expected = sizeof(rsc); break;
    case 0x81c60000: index = 3; base = db; expected = sizeof(db); break;
    case 0x01d04000: index = 4; base = sm_pages; expected = sizeof(sm_pages); break;
    case 0x01d00000: index = 5; base = crm_pages; expected = sizeof(crm_pages); break;
    default: assert(!"unexpected physical mapping"); return 0;
    }
    assert(size == expected);
    if (pa == failed_pa) return 0;
    assert(!(mapped & (1u << index))); mapped |= 1u << index;
    return (uintptr_t)base;
}
void ml_io_unmap(vm_offset_t address, vm_size_t size)
{
    void *bases[] = {gcc, gpio, rsc, db, sm_pages, crm_pages};
    unsigned sizes[] = {sizeof(gcc), sizeof(gpio), sizeof(rsc), sizeof(db), sizeof(sm_pages), sizeof(crm_pages)};
    assert(!(address & (PAGE_SIZE - 1u)) && !(size & (PAGE_SIZE - 1u)));
    for (unsigned i = 0; i < 6; ++i) if ((uintptr_t)bases[i] == address) {
        assert(size == sizes[i] && (mapped & (1u << i)) && !(unmapped & (1u << i)));
        unmapped |= 1u << i; return;
    }
    assert(!"unmap outside owned maps");
}
kern_return_t thread_terminate(thread_t t) { assert(t == current_thread()); ++exits; return 0; }
void loader_log_line(const char *text)
{
    ++lines;
    if (strstr(text, "early SM +0x1040=")) assert(strstr(text, "=0x12345678"));
    if (strstr(text, "early CRM +0=")) assert(strstr(text, "=0x10200"));
    if (strstr(text, "read-only probe complete")) saw_complete = 1;
    if (strstr(text, "early CESTA/CRM headers captured")) saw_noc = 1;
}
int giulia_rpmh_resolve_vrm(const uint8_t *data, uint32_t bytes, uint32_t version,
    const char *name, uint32_t *address)
{
    assert(data == (const uint8_t *)db && bytes == sizeof(db) && version == db[0]);
    assert(name && address); ++resolver_calls; *address = 0;
    return GIULIA_RPMH_NOT_FOUND;
}
static uint64_t fingerprint(void)
{
    uint32_t *bases[] = {gcc, gpio, rsc, db, sm_pages, crm_pages};
    unsigned words[] = {sizeof(gcc)/4, sizeof(gpio)/4, sizeof(rsc)/4, sizeof(db)/4, sizeof(sm_pages)/4, sizeof(crm_pages)/4};
    uint64_t h = 1469598103934665603ull;
    for (unsigned i = 0; i < 6; ++i)
        for (unsigned j = 0; j < words[i]; ++j) h = (h ^ bases[i][j]) * 1099511628211ull;
    return h;
}
static void setup(unsigned requested)
{
    mode = requested; board = HACKIOS_PLATFORM_GIULIA;
    creates = exits = deallocs = lines = mapped = unmapped = failed_pa = 0;
    thread_error = saw_complete = saw_noc = resolver_calls = 0;
    wifi_probe_started = wifi_probe_mode = 0;
    memset(db, 0, sizeof(db));
    memset(sm_pages, 0, sizeof(sm_pages)); memset(crm_pages, 0, sizeof(crm_pages));
    sm_pages[0] = crm_pages[0] = UINT32_MAX; /* Allocation != resource base. */
    sm[0x1040 / 4] = 0x12345678;
    db[0] = 1; db[1] = 0x0c0330db;
    crm[0] = 0x00010200; /* Synthetic layout identity, not measured device. */
    crm[1] = (3u<<24) | (1u<<20) | (2u<<16) | (2u<<12) | (2u<<8) | 0x25;
    crm[2] = 6u<<26; crm[3] = 1;
    original = fingerprint();
}
static void check(void)
{
    assert(mapped == unmapped && fingerprint() == original);
    assert(exits == 1 && deallocs == 1);
}
int main(void)
{
    setup(0); giulia_wifi_init(); assert(!creates && !mapped);
    setup(3); giulia_wifi_init(); assert(!creates && !mapped);
    setup(1); board = 1; giulia_wifi_init(); assert(!creates && !mapped);
    setup(1); giulia_wifi_init(); check();
    assert(mapped == 15 && saw_complete && !saw_noc && resolver_calls == 7);
    giulia_wifi_init(); assert(creates == 1);
    setup(2); giulia_wifi_init(); check(); assert(mapped == 63 && saw_noc && saw_complete);
    static const uint32_t fail[] = {0x100000,0x0f100000,0x17a20000,0x81c60000,0x01d04000,0x01d00000};
    for (unsigned i = 0; i < sizeof(fail)/sizeof(fail[0]); ++i) {
        setup(2); failed_pa = fail[i]; giulia_wifi_init(); check(); assert(!saw_complete);
    }
    setup(2); crm[3] = UINT32_MAX; original = fingerprint();
    giulia_wifi_init(); check(); assert(!saw_noc && !saw_complete);
    setup(1); db[1] = 0; original = fingerprint();
    giulia_wifi_init(); check(); assert(!resolver_calls && saw_complete);
    setup(1); thread_error = 1; giulia_wifi_init(); assert(creates == 1 && !mapped && !exits);
    puts("Ace5 XNU Wi-Fi probe: opt-in, board gate, read-only maps and cleanup passed");
    return 0;
}
