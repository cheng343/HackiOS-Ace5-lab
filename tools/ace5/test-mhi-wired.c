/* SPDX-License-Identifier: BSD-2-Clause */
/* Compile the real backing implementation against CPU-only mock VM services. */
#ifdef NDEBUG
#error "This fixture requires assertions"
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "giulia_mhi_wired.h"
typedef uintptr_t vm_offset_t;
static unsigned native_page, allocs, frees, cleans, fail_alloc, bad_mode, bad_slice;
static uintptr_t last_clean;
static uint32_t last_clean_bytes;
#define PAGE_SIZE native_page
#define KMA_KOBJECT 1
#define KMA_ZERO 2
#define KMA_DATA 4
#define KMA_GUARD_FIRST 8
#define KMA_GUARD_LAST 16
#define VM_KERN_MEMORY_IOKIT  27
#define KERN_SUCCESS 0
#define kernel_map ((void *)&native_page)
#define ROOM (GIULIA_MHI_WIRED_MAX + 2u*16384u)
struct fake { _Alignas(16384) uint8_t area[ROOM]; uint32_t size; };
static struct fake maps[2];
static int kmem_alloc(void *map,vm_offset_t *result,uint32_t size,unsigned flags,unsigned tag)
{
    unsigned expected=KMA_ZERO|KMA_DATA|KMA_GUARD_FIRST|KMA_GUARD_LAST;
    assert(map==kernel_map && result && tag==VM_KERN_MEMORY_IOKIT);
    assert((flags & ~KMA_KOBJECT)==expected && size>=3*PAGE_SIZE && size<=ROOM && !(size&(PAGE_SIZE-1u)));
    allocs++; if (fail_alloc) return -1;
    for(unsigned j=0;j<2;j++) if (!maps[j].size) {
        struct fake *m=&maps[j]; m->size=size;
        memset(m->area,0xa5,size);
        memset(m->area+PAGE_SIZE,0,size-2*PAGE_SIZE);
        *result=(uintptr_t)m->area; return 0;
    }
    return -1;
}
static void kmem_free(void *map,vm_offset_t base,uint32_t size)
{
    assert(map==kernel_map);
    for(unsigned j=0;j<2;j++) if (maps[j].size && base==(uintptr_t)maps[j].area) {
        assert(size==maps[j].size);
        for(unsigned i=0;i<PAGE_SIZE;i++) assert(maps[j].area[i]==0xa5 && maps[j].area[size-PAGE_SIZE+i]==0xa5);
        maps[j].size=0; frees++; return;
    }
    assert(0 && "release requires the original complete guarded VM entry");
}
static unsigned lookup(vm_offset_t va,uint32_t *off)
{
    for(unsigned j=0;j<2;j++) {
        uintptr_t start=(uintptr_t)maps[j].area+PAGE_SIZE;
        if (maps[j].size && va>=start && va-start<maps[j].size-2*PAGE_SIZE) {
            *off=(uint32_t)(va-start); return j;
        }
    }
    assert(0 && "data access outside exclusive wired range"); return 0;
}
static uint64_t mock_pa(unsigned slot,uint32_t off)
{
    /* Native VM pages are not assumed consecutive in physical RAM. */
    return UINT64_C(0x160000000)+slot*UINT64_C(0x01000000)+(off/PAGE_SIZE)*UINT64_C(0x00100000)+off%PAGE_SIZE;
}
static uint64_t ml_vtophys(vm_offset_t va)
{
    uint32_t off=0; unsigned j=lookup(va,&off);
    if (off/4096u==bad_slice) {
        if (bad_mode==1 && !(off%4096u)) return 0;
        if (bad_mode==2 && !(off%4096u)) return mock_pa(j,off)+4;
        if (bad_mode==3 && off%4096u==4095) return mock_pa(j,off)+4096;
    }
    return mock_pa(j,off);
}
static void CleanPoC_DcacheRegion_Force(vm_offset_t va,uint32_t count)
{
    uint32_t a=0,b=0; const unsigned j=lookup(va,&a);
    assert(count && j==lookup(va+count-1,&b) && b-a==count-1);
    last_clean=va; last_clean_bytes=count; cleans++;
}
#define GIULIA_MHI_WIRED_HOST_TEST 1
#include "giulia_mhi_wired.c"

static void one_size(unsigned pg)
{
    struct giulia_mhi_wired a={0}, b={0}; native_page=pg;
    unsigned af=allocs, ff=frees, cf=cleans;
    assert(giulia_mhi_wired_alloc(&a,0)!=0 && giulia_mhi_wired_alloc(&a,UINT32_MAX)!=0 && allocs==af);
    fail_alloc=1; assert(giulia_mhi_wired_alloc(&a,1)!=0 && !a.self && frees==ff && cleans==cf); fail_alloc=0;
    for(unsigned fault=1;fault<=3;fault++) {
        bad_mode=fault; bad_slice=2;
        assert(giulia_mhi_wired_alloc(&a,pg+8193)!=0 && !a.base && !a.self);
        assert(frees==ff+fault && !maps[0].size && cleans==cf);
    }
    bad_mode=0;
    uint32_t size=pg+37, used=0; void *ptr=NULL; uint64_t pa=0,again=0;
    assert(!giulia_mhi_wired_alloc(&a,size));
    assert(!giulia_mhi_wired_cpu(&a,&ptr,&used) && ptr==maps[0].area+pg && used==size);
    assert(last_clean==(uintptr_t)ptr && last_clean_bytes==2*pg);
    for(unsigned i=0;i<2*pg;i++) assert(!((uint8_t *)ptr)[i]);
    for(unsigned i=0;i<size;i+=4096) {
        assert(!giulia_mhi_wired_phys4k(&a,i,&pa) && pa==mock_pa(0,i));
        assert(pa!=(uintptr_t)ptr+i);
    }
    pa=123;
    assert(giulia_mhi_wired_phys4k(&a,1,&pa)!=0 && pa==123);
    assert(giulia_mhi_wired_phys4k(&a,pg+4096,&pa)!=0 && pa==123);
    bad_mode=3;bad_slice=0;
    assert(giulia_mhi_wired_phys4k(&a,0,&pa)!=0 && pa==123);bad_mode=0;
    assert(!giulia_mhi_wired_alloc(&b,1)); /* no other object can own the tail of a's native page */
    assert(!giulia_mhi_wired_phys4k(&b,0,&again) && again==mock_pa(1,0));
    assert(last_clean==(uintptr_t)maps[1].area+pg && last_clean_bytes==pg);
    ((uint8_t *)ptr)[size-1]=42;
    assert(!giulia_mhi_wired_clean(&a,size-1,1));
    unsigned count=cleans; assert(last_clean==(uintptr_t)ptr+size-1 && last_clean_bytes==1);
    assert(giulia_mhi_wired_clean(&a,size-1,2)!=0 && giulia_mhi_wired_clean(&a,0,0)!=0 &&
        giulia_mhi_wired_clean(&a,UINT32_MAX,32)!=0 && cleans==count);
    struct giulia_mhi_wired copy=a;
    assert(giulia_mhi_wired_release(&copy)!=0 && giulia_mhi_wired_phys4k(&copy,0,&pa)!=0);
    assert(giulia_mhi_wired_alloc(&a,32)!=0 && !giulia_mhi_wired_release(&b) && maps[0].size);
    assert(!giulia_mhi_wired_release(&a) && !giulia_mhi_wired_release(&a));
    assert(frees==ff+5 && !maps[0].size && !maps[1].size);
    ptr=(void *)&a; used=99;
    assert(giulia_mhi_wired_cpu(&a,&ptr,&used)!=0 && ptr==(void *)&a && used==99);
}
int main(void)
{
    struct giulia_mhi_wired closed={0};
    native_page=8192; assert(giulia_mhi_wired_alloc(&closed,1)!=0 && !allocs);
    one_size(4096); one_size(16384);
    puts("PASS: guarded XNU wired backing validates sparse 4K physical slices, native-page zero and clean, rollback and copy rejection");
    return 0;
}
