/* SPDX-License-Identifier: BSD-2-Clause */
/* OnePlusOSS SM8650 66305c56, drivers/iommu/io-pgtable-fast.c
 * av8l_fast_prot_to_pte, av8l_fast_prepopulate_pgtables, av8l_fast_alloc_pgtable.
 * This independent builder deliberately uses read-only normal cacheable TX
 * leaves, not a claim that stock's coherent allocations themselves are RO.
 * Little-endian descriptor stores below are for INACTIVE tables only. */
#include "giulia_mhi_s1.h"

#define S1_ROOT_INDEX 2u
#define S1_MIDDLE_FIRST 192u
#define S1_ENTRIES 512u
#define S1_PA_MASK 0x0000fffffffff000ULL
#define S1_TX 0x0060000000000ec7ULL /* PXN/UXN,nG,AF,outer-shareable,RO,AttrIdx1,page */
#define S1_TABLE 3ULL

static uint64_t s1_addr(const void *p) { return (uint64_t)(uintptr_t)p; }
static int s1_overlap(uint64_t a,uint64_t an,uint64_t b,uint64_t bn)
{
    return a <= b ? b-a < an : a-b < bn;
}
static int s1_range(uint64_t a,uint64_t n,uint64_t last)
{
    return n && a <= last && n-1u <= last-a;
}
static uint64_t s1_read(const uint8_t *page,uint32_t i)
{
    const uint8_t *p=page+(uint64_t)i*8u;
    uint64_t v=0;
    for (uint32_t j=0;j<8u;j++) v |= (uint64_t)p[j] << (8u*j);
    return v;
}
static void s1_write(uint8_t *page,uint32_t i,uint64_t v)
{
    uint8_t *p=page+(uint64_t)i*8u;
    for (uint32_t j=0;j<8u;j++) { p[j]=(uint8_t)v; v >>= 8; }
}
static int s1_zero(const uint8_t *p)
{
    for (uint32_t i=0;i<GIULIA_MHI_S1_PAGE;i++) if (p[i]) return 0;
    return 1;
}
static int s1_width(uint32_t w)
{
    return w==32u || w==36u || w==40u || w==42u || w==44u || w==48u;
}

int giulia_mhi_s1_init(struct giulia_mhi_s1 *s,const struct giulia_mhi_s1_ops *ops,
    const struct giulia_mhi_s1_page *pages,uint32_t count,uint32_t oas_bits)
{
    uint64_t ptr_last=(uint64_t)~(uintptr_t)0;
    if (!s || !ops || !pages || count<3u || count>GIULIA_MHI_S1_MAX_TABLES ||
        !s1_width(oas_bits)) return GIULIA_MHI_S1_ARGUMENT;
    uint64_t list_bytes=(uint64_t)count*sizeof(*pages);
    if (!s1_range(s1_addr(s),sizeof(*s),ptr_last) ||
        !s1_range(s1_addr(ops),sizeof(*ops),ptr_last) ||
        !s1_range(s1_addr(pages),list_bytes,ptr_last) ||
        s1_overlap(s1_addr(s),sizeof(*s),s1_addr(ops),sizeof(*ops)) ||
        s1_overlap(s1_addr(s),sizeof(*s),s1_addr(pages),list_bytes) ||
        s1_overlap(s1_addr(ops),sizeof(*ops),s1_addr(pages),list_bytes) ||
        !ops->owns_table || !ops->owns_tx) return GIULIA_MHI_S1_ARGUMENT;
    if (s->page_count) return GIULIA_MHI_S1_STATE;
    uint64_t limit=(1ULL << oas_bits)-1u;
    for (uint32_t i=0;i<count;i++) {
        uint64_t a=s1_addr(pages[i].cpu), pa=pages[i].pa;
        if (!a || (a & 4095u) || (pa & 4095u) ||
            !s1_range(a,4096u,ptr_last) || !s1_range(pa,4096u,limit))
            return GIULIA_MHI_S1_RANGE;
        if (s1_overlap(a,4096u,s1_addr(s),sizeof(*s)) ||
            s1_overlap(a,4096u,s1_addr(ops),sizeof(*ops)) ||
            s1_overlap(a,4096u,s1_addr(pages),list_bytes))
            return GIULIA_MHI_S1_CONFLICT;
        for (uint32_t j=0;j<i;j++)
            if (pages[j].pa==pa || pages[j].cpu==pages[i].cpu) return GIULIA_MHI_S1_CONFLICT;
        if (ops->owns_table(ops->cookie,&pages[i])) return GIULIA_MHI_S1_OWNER;
        if (!s1_zero(pages[i].cpu)) return GIULIA_MHI_S1_CONFLICT;
    }
    /* No failure beyond this point. Valid covers 32-bit IAS; only this
     * single 1GiB branch is created, not neighbouring IOVA regions. */
    s->ops=*ops;
    s->pa_last=limit;
    s->used=2u;
    __builtin_memset(s->leaf,0,sizeof(s->leaf));
    __builtin_memcpy(s->table,pages,(unsigned long)list_bytes);
    s1_write(s->table[0].cpu,S1_ROOT_INDEX,s->table[1].pa|S1_TABLE);
    s->page_count=count;
    return GIULIA_MHI_S1_OK;
}

static int s1_owned(struct giulia_mhi_s1 *s)
{
    if (!s || s->page_count<3u || s->page_count>GIULIA_MHI_S1_MAX_TABLES ||
        s->used<2u || s->used>s->page_count) return GIULIA_MHI_S1_STATE;
    for (uint32_t i=0;i<s->page_count;i++)
        if (s->ops.owns_table(s->ops.cookie,&s->table[i])) return GIULIA_MHI_S1_OWNER;
    return GIULIA_MHI_S1_OK;
}

static int s1_change(struct giulia_mhi_s1 *s,uint64_t iova,uint64_t pa,
    uint32_t pages,int remove)
{
    int rc=s1_owned(s);
    if (rc) return rc;
    uint64_t size=(uint64_t)pages*4096u;
    if (!pages || ((iova|pa) & 4095u) || iova<GIULIA_MHI_S1_FIRST ||
        !s1_range(iova,size,GIULIA_MHI_S1_LAST) || !s1_range(pa,size,s->pa_last))
        return GIULIA_MHI_S1_RANGE;
    if (s->ops.owns_tx(s->ops.cookie,pa,pages)) return GIULIA_MHI_S1_OWNER;
    /* Numeric PA aliases with even UNUSED table-pool pages are forbidden. */
    for (uint32_t i=0;i<s->page_count;i++)
        if (s1_overlap(pa,size,s->table[i].pa,4096u)) return GIULIA_MHI_S1_CONFLICT;
    if (s1_read(s->table[0].cpu,S1_ROOT_INDEX)!=(s->table[1].pa|S1_TABLE))
        return GIULIA_MHI_S1_CONFLICT;
    uint32_t first=(uint32_t)((iova-GIULIA_MHI_S1_FIRST)>>12);
    uint32_t leaf_first=first/S1_ENTRIES, leaf_last=(first+pages-1u)/S1_ENTRIES;
    uint32_t missing=0;
    /* Phase 1: the entire request, including the LAST entry of a multi-leaf
     * operation, is checked before linking a single new table or writing a PTE. */
    for (uint32_t n=leaf_first;n<=leaf_last;n++) {
        uint32_t idx=s->leaf[n];
        uint64_t middle=s1_read(s->table[1].cpu,S1_MIDDLE_FIRST+n);
        if (idx==0u) {
            if (middle || remove) return GIULIA_MHI_S1_CONFLICT;
            missing++;
        } else if (idx<2u || idx>=s->used || middle!=(s->table[idx].pa|S1_TABLE))
            return GIULIA_MHI_S1_CONFLICT;
    }
    if (missing > s->page_count-s->used) return GIULIA_MHI_S1_CAPACITY;
    for (uint32_t i=0;i<missing;i++)
        if (!s1_zero(s->table[s->used+i].cpu)) return GIULIA_MHI_S1_CONFLICT;
    for (uint32_t i=0;i<pages;i++) {
        uint32_t pos=first+i, idx=s->leaf[pos/S1_ENTRIES];
        uint64_t got=idx ? s1_read(s->table[idx].cpu,pos%S1_ENTRIES) : 0;
        uint64_t wanted=remove ? ((pa+(uint64_t)i*4096u)|S1_TX) : 0;
        if (got!=wanted) return GIULIA_MHI_S1_CONFLICT;
    }
    /* One bounded scan per contiguous PA run, not one scan per new page. */
    if (!remove) for (uint32_t idx=2u;idx<s->used;idx++) {
        for (uint32_t j=0;j<S1_ENTRIES;j++) {
            uint64_t v=s1_read(s->table[idx].cpu,j);
            if (v && ((v & ~S1_PA_MASK)!=S1_TX ||
                s1_overlap(pa,size,v&S1_PA_MASK,4096u))) return GIULIA_MHI_S1_CONFLICT;
        }
    }
    /* Phase 2: pinning and serialization exclude ownership changes; no
     * fallible operations and no external hardware walks occur in this phase. */
    for (uint32_t n=leaf_first;n<=leaf_last;n++) {
        if (s->leaf[n]) continue;
        uint32_t idx=s->used++;
        s->leaf[n]=(uint8_t)idx;
        s1_write(s->table[1].cpu,S1_MIDDLE_FIRST+n,s->table[idx].pa|S1_TABLE);
    }
    for (uint32_t i=0;i<pages;i++) {
        uint32_t pos=first+i, idx=s->leaf[pos/S1_ENTRIES];
        s1_write(s->table[idx].cpu,pos%S1_ENTRIES,remove ? 0 : ((pa+(uint64_t)i*4096u)|S1_TX));
    }
    return GIULIA_MHI_S1_OK;
}
int giulia_mhi_s1_map(struct giulia_mhi_s1 *s,uint64_t iova,uint64_t pa,uint32_t pages)
{
    return s1_change(s,iova,pa,pages,0);
}
int giulia_mhi_s1_unmap(struct giulia_mhi_s1 *s,uint64_t iova,uint64_t pa,uint32_t pages)
{
    return s1_change(s,iova,pa,pages,1);
}
int giulia_mhi_s1_reset(struct giulia_mhi_s1 *s)
{
    if (!s) return GIULIA_MHI_S1_ARGUMENT;
    if (!s->page_count) return GIULIA_MHI_S1_OK;
    int rc=s1_owned(s);
    if (rc) return rc; /* no partial wipe after lost pool ownership */
    for (uint32_t i=0;i<s->used;i++) __builtin_memset(s->table[i].cpu,0,4096u);
    __builtin_memset(s,0,sizeof(*s));
    return GIULIA_MHI_S1_OK;
}
