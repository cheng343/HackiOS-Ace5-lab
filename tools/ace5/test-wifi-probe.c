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
static int thread_error, saw_complete, saw_noc, saw_aop, saw_full, resolver_calls;
static unsigned arc_lines,saw_arc_votes;
static _Alignas(0x4000) uint32_t gcc[0x70000 / 4], gpio[0x100000 / 4], rsc[0x10000 / 4];
static _Alignas(0x4000) uint32_t db[0x20000 / 4], sm_pages[0xc000 / 4], crm_pages[0x4000 / 4];
static _Alignas(0x4000) uint32_t aop_pages[0x4000 / 4];
static _Alignas(0x4000) uint32_t arc_pages[0x10000 / 4];
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
    case 0x0c300000: index = 6; base = aop_pages; expected = sizeof(aop_pages); break;
    case 0x0b620000: index = 7; base = arc_pages; expected = sizeof(arc_pages); break;
    default: assert(!"unexpected physical mapping"); return 0;
    }
    assert(size == expected);
    if (pa == failed_pa) return 0;
    assert(!(mapped & (1u << index))); mapped |= 1u << index;
    return (uintptr_t)base;
}
void ml_io_unmap(vm_offset_t address, vm_size_t size)
{
    void *bases[] = {gcc, gpio, rsc, db, sm_pages, crm_pages, aop_pages,arc_pages};
    unsigned sizes[] = {sizeof(gcc), sizeof(gpio), sizeof(rsc), sizeof(db), sizeof(sm_pages), sizeof(crm_pages), sizeof(aop_pages),sizeof(arc_pages)};
    assert(!(address & (PAGE_SIZE - 1u)) && !(size & (PAGE_SIZE - 1u)));
    for (unsigned i = 0; i < 8; ++i) if ((uintptr_t)bases[i] == address) {
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
    if (strstr(text, "AOP descriptor captured")) saw_aop = 1;
    if (strstr(text, "full SM/CRM snapshot captured; no table/channel writes")) saw_full = 1;
    if (strstr(text,"HLOS ARC snapshot captured; resource ownership NOT assumed")) saw_arc_votes=1;
    if (strstr(text,"ARC cx.lvl rc=0")) {
        assert(strstr(text,"selector=2 corner=64 (prior vote unknown)")); arc_lines++;
    }
    if (strstr(text,"ARC xo.lvl rc=0")) {
        assert(strstr(text,"selector=3 corner=128 (prior vote unknown)")); arc_lines++;
    }
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
    uint32_t *bases[] = {gcc, gpio, rsc, db, sm_pages, crm_pages, aop_pages,arc_pages};
    unsigned words[] = {sizeof(gcc)/4, sizeof(gpio)/4, sizeof(rsc)/4, sizeof(db)/4, sizeof(sm_pages)/4, sizeof(crm_pages)/4, sizeof(aop_pages)/4,sizeof(arc_pages)/4};
    uint64_t h = 1469598103934665603ull;
    for (unsigned i = 0; i < 8; ++i)
        for (unsigned j = 0; j < words[i]; ++j) h = (h ^ bases[i][j]) * 1099511628211ull;
    return h;
}
static void setup(unsigned requested)
{
    mode = requested; board = HACKIOS_PLATFORM_GIULIA;
    creates = exits = deallocs = lines = mapped = unmapped = failed_pa = 0;
    thread_error = saw_complete = saw_noc = saw_aop = saw_full = resolver_calls = 0;
    arc_lines=saw_arc_votes=0;
    wifi_probe_started = wifi_probe_mode = 0;
    memset(db, 0, sizeof(db));
    memset(sm_pages, 0, sizeof(sm_pages)); memset(crm_pages, 0, sizeof(crm_pages));
    sm_pages[0] = crm_pages[0] = UINT32_MAX; /* Allocation != resource base. */
    sm[0x1040 / 4] = 0x12345678;
    db[0] = 1; db[1] = 0x0c0330db;
    /* Synthetic ARC entries exercise the real bounded parser in XNU probe. */
    uint16_t *header=(uint16_t *)((uint8_t *)db+8);
    header[0]=3; header[2]=128; header[3]=2;
    for (unsigned i=0;i<2;i++) {
        uint8_t *e=(uint8_t *)db+144+i*24;
        memcpy(e,i ? "cx.lvl":"xo.lvl",6);
        uint32_t addr=0x30080+i*16; memcpy(e+16,&addr,4);
        uint16_t len=8,offset=(uint16_t)(i*8);
        memcpy(e+20,&len,2); memcpy(e+22,&offset,2);
        const uint16_t levels[2][4]={{0,32,80,128},{0,16,64,128}};
        memcpy((uint8_t *)db+272+i*8,levels[i],8);
    }
    crm[0] = 0x00010200; /* Synthetic layout identity, not measured device. */
    crm[1] = (3u<<24) | (1u<<20) | (2u<<16) | (2u<<12) | (2u<<8) | 0x25;
    crm[2] = 6u<<26; crm[3] = 1;
    memset(aop_pages, 0, sizeof(aop_pages));
    aop_pages[0] = 0x4d41494c; aop_pages[1] = 1;
    aop_pages[0x34/4] = 0x100; aop_pages[0x38/4] = 0x100;
    memset(arc_pages,0,sizeof(arc_pages));
    arc_pages[0x80/4]=3; arc_pages[0x90/4]=2;
    original = fingerprint();
}
static void check(void)
{
    assert(mapped == unmapped && fingerprint() == original);
    assert(exits == 1 && deallocs == 1);
}
struct arc_fake { unsigned reads,fail,change; };
static int arc_read(void *cookie,uint32_t off,uint32_t *v)
{
    struct arc_fake *f=cookie;
    const uint32_t sequence[]={0x80,0x90,0x90,0x80};
    assert(f->reads<4 && off==sequence[f->reads]);
    f->reads++;
    if (f->reads==f->fail) return -91;
    *v=arc_pages[off/4] ^ (f->reads==f->change ? 0x100u:0);
    return 0;
}
static void arc_snapshot_core(void)
{
    struct giulia_arc_votes s;
    struct arc_fake f={0};
    struct giulia_arc_votes_io io={&f,arc_read,GIULIA_ARC_HLOS_PHYS,GIULIA_ARC_HLOS_SIZE};
    setup(5);
    assert(!giulia_arc_votes_snapshot(&io,(const uint8_t *)db,sizeof(db),1,&s));
    assert(s.valid && s.reads==4 && s.selector[0]==3 && s.selector[1]==2);
    for (unsigned n=1;n<=4;n++) {
        f=(struct arc_fake){.fail=n};
        assert(giulia_arc_votes_snapshot(&io,(const uint8_t *)db,sizeof(db),1,&s)==GIULIA_ARC_VOTES_IO);
        assert(!s.valid && s.reads==n && s.callback_error==-91);
        f=(struct arc_fake){.change=n};
        assert(giulia_arc_votes_snapshot(&io,(const uint8_t *)db,sizeof(db),1,&s)==GIULIA_ARC_VOTES_CHANGED);
        assert(!s.valid && s.reads==4);
    }
    for (unsigned mode=0;mode<8;mode++) {
        setup(5); f=(struct arc_fake){0};
        if (mode==0) io.phys+=0x10000; /* A different master is never queried. */
        if (mode==1) io.size-=4;
        if (mode==2) db[1]=0;
        uint32_t address=mode==3 ? 0x30084u:mode==4 ? 0x300e0u:0x30090u;
        if (mode>=3 && mode<=5) memcpy((uint8_t *)db+144+16,&address,4);
        if (mode==6) arc_pages[0x80/4]=4;
        if (mode==7) arc_pages[0x80/4]=UINT32_MAX;
        assert(giulia_arc_votes_snapshot(&io,(const uint8_t *)db,sizeof(db),1,&s)<0 && !s.valid);
        assert(f.reads==(mode>=6 ? 1u:0u));
        io.phys=GIULIA_ARC_HLOS_PHYS; io.size=GIULIA_ARC_HLOS_SIZE;
    }
    puts("Ace5 HLOS ARC votes: real parser, exact row, bounded reads, invalid selectors and drift rejected");
}
int main(void)
{
    setup(0); giulia_wifi_init(); assert(!creates && !mapped);
    setup(6); giulia_wifi_init(); assert(!creates && !mapped);
    setup(1); board = 1; giulia_wifi_init(); assert(!creates && !mapped);
    setup(1); giulia_wifi_init(); check();
    assert(mapped == 15 && saw_complete && !saw_noc && resolver_calls == 7);
    assert(arc_lines==2);
    giulia_wifi_init(); assert(creates == 1);
    setup(2); giulia_wifi_init(); check(); assert(mapped == 63 && saw_noc && saw_complete);
    setup(3); giulia_wifi_init(); check(); assert(mapped == 127 && saw_noc && saw_aop && saw_complete);
    setup(4); giulia_wifi_init(); check(); assert(mapped == 127 && saw_full && saw_aop && saw_complete);
    setup(5); giulia_wifi_init(); check();
    assert(mapped==143 && saw_arc_votes && saw_complete && !saw_noc && !saw_aop);
    setup(5); failed_pa=0xb620000; giulia_wifi_init(); check(); assert(!saw_arc_votes && !saw_complete);
    setup(5); arc_pages[0x90/4]=UINT32_MAX; original=fingerprint();
    giulia_wifi_init(); check(); assert(!saw_arc_votes && !saw_complete);
    setup(4); sm[0x1078/4] = UINT32_MAX; original = fingerprint();
    giulia_wifi_init(); check(); assert(saw_aop && !saw_full && !saw_complete);
    setup(4); crm[1] ^= 0x10; original = fingerprint();
    giulia_wifi_init(); check(); assert(!saw_full && !saw_complete);
    setup(3); failed_pa = 0x0c300000; giulia_wifi_init(); check(); assert(!saw_aop && !saw_complete);
    setup(3); aop_pages[1] = 2; original = fingerprint();
    giulia_wifi_init(); check(); assert(!saw_aop && !saw_complete);
    static const uint32_t fail[] = {0x100000,0x0f100000,0x17a20000,0x81c60000,0x01d04000,0x01d00000};
    for (unsigned i = 0; i < sizeof(fail)/sizeof(fail[0]); ++i) {
        setup(2); failed_pa = fail[i]; giulia_wifi_init(); check(); assert(!saw_complete);
    }
    setup(2); crm[3] = UINT32_MAX; original = fingerprint();
    giulia_wifi_init(); check(); assert(!saw_noc && !saw_complete);
    setup(1); db[1] = 0; original = fingerprint();
    giulia_wifi_init(); check(); assert(!resolver_calls && !arc_lines && saw_complete);
    setup(1); thread_error = 1; giulia_wifi_init(); assert(creates == 1 && !mapped && !exits);
    puts("Ace5 XNU Wi-Fi probe: opt-in, board gate, read-only maps and cleanup passed");
    puts("Ace5 XNU NoC snapshot: actual full capture, read failures and untouched MMIO passed");
    puts("Ace5 XNU ARC probe: real level parser, selector diagnostics and untouched MMIO passed");
    arc_snapshot_core();
    puts("Ace5 XNU HLOS ARC probe: opt-in mode5, no SM/CRM/AOP access and untouched MMIO passed");
    return 0;
}
