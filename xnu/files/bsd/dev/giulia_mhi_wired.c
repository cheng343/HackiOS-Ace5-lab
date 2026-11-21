/* SPDX-License-Identifier: BSD-2-Clause */
/* ARM64 XNU backing for detached KIWI data and tables. No device/SMMU access. */
#include "giulia_mhi_wired.h"
#ifndef GIULIA_MHI_WIRED_HOST_TEST
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <machine/machine_routines.h>
#include <arm/caches_internal.h>
#include <vm/vm_kern.h>
#include <vm/memory_types.h>
#endif

static uint32_t gw_round(uint32_t n, uint32_t page)
{
    return (n + page - 1u) & ~(page - 1u);
}
static int gw_empty(const struct giulia_mhi_wired *w)
{
    return w && !w->self && !w->base && !w->bytes && !w->extent && !w->page;
}
static int gw_valid(const struct giulia_mhi_wired *w)
{
    return w && w->self == w && (w->page == 4096 || w->page == 16384) &&
        w->bytes && w->bytes <= GIULIA_MHI_WIRED_MAX &&
        w->extent == gw_round(w->bytes, w->page) && w->base &&
        !(w->base & (w->page - 1u)) &&
        w->base <= UINTPTR_MAX - (w->extent + 2u*w->page - 1u);
}
static int gw_phys(uintptr_t va, uint64_t *out)
{
    uint64_t pa = ml_vtophys((vm_offset_t)va);
    if (!pa || (pa & (GIULIA_MHI_WIRED_SLICE - 1u)) ||
        ml_vtophys((vm_offset_t)(va + GIULIA_MHI_WIRED_SLICE - 1u)) !=
        pa + GIULIA_MHI_WIRED_SLICE - 1u) return -1;
    *out = pa;
    return 0;
}
int giulia_mhi_wired_alloc(struct giulia_mhi_wired *w, uint32_t bytes)
{
    uint32_t page = (uint32_t)PAGE_SIZE;
    if (!gw_empty(w) || !bytes || bytes > GIULIA_MHI_WIRED_MAX ||
        (page != 4096 && page != 16384)) return -1;
    uint32_t extent = gw_round(bytes, page), total = extent + 2u*page;
    vm_offset_t base = 0;
    /* XNU includes BOTH guard pages in the size; it returns the first guard
     * address, NOT the first accessible byte. No VAONLY/PAGEABLE flags. */
    if (kmem_alloc(kernel_map, &base, total, KMA_KOBJECT | KMA_ZERO | KMA_DATA |
        KMA_GUARD_FIRST | KMA_GUARD_LAST, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) return -1;
    /* A successful kmem_alloc promises a complete aligned entry. Do not
     * release a malformed range if the kernel itself breaks that contract. */
    if (!base || (base & (page - 1u)) || base > UINTPTR_MAX-(total-1u)) return -1;
    for (uint32_t off = 0; off < extent; off += GIULIA_MHI_WIRED_SLICE) {
        uint64_t physical;
        if (gw_phys(base + page + off, &physical)) {
            kmem_free(kernel_map, base, total);
            return -1;
        }
    }
    /* KMA_ZERO clears the WHOLE exclusive native extent. Publish zero padding
     * to PoC before any address is available, including 16K-page tail space. */
    CleanPoC_DcacheRegion_Force(base + page, extent);
    *w = (struct giulia_mhi_wired){w, (uintptr_t)base, bytes, extent, page};
    return 0;
}
int giulia_mhi_wired_cpu(const struct giulia_mhi_wired *w, void **cpu, uint32_t *bytes)
{
    if (!gw_valid(w) || !cpu || !bytes) return -1;
    *cpu = (void *)(w->base + w->page);
    *bytes = w->bytes;
    return 0;
}
int giulia_mhi_wired_phys4k(const struct giulia_mhi_wired *w, uint32_t off, uint64_t *pa)
{
    if (!gw_valid(w) || !pa || (off & (GIULIA_MHI_WIRED_SLICE - 1u)) ||
        off >= w->bytes) return -1;
    return gw_phys(w->base + w->page + off, pa);
}
int giulia_mhi_wired_clean(const struct giulia_mhi_wired *w, uint32_t off, uint32_t bytes)
{
    if (!gw_valid(w) || !bytes || off >= w->bytes || bytes > w->bytes - off) return -1;
    CleanPoC_DcacheRegion_Force(w->base + w->page + off, bytes);
    return 0;
}
int giulia_mhi_wired_release(struct giulia_mhi_wired *w)
{
    if (gw_empty(w)) return 0;
    if (!gw_valid(w)) return -1;
    kmem_free(kernel_map, (vm_offset_t)w->base, w->extent + 2u*w->page);
    *w = (struct giulia_mhi_wired){0};
    return 0;
}
