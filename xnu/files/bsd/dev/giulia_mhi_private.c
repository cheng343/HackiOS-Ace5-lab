/* SPDX-License-Identifier: BSD-2-Clause */
#include "giulia_mhi_private.h"

_Static_assert(GIULIA_MHI_S1_PAGE == GIULIA_MHI_WIRED_SLICE, "I/O granule");
_Static_assert(GIULIA_MHI_S1_FIRST == GIULIA_MHI_STAGE_FIRST &&
    GIULIA_MHI_S1_LAST == GIULIA_MHI_STAGE_LAST, "KIWI data aperture");
_Static_assert(GIULIA_MHI_WIRED_MAX == GIULIA_MHI_FW_SEGMENT_BYTES, "segment");

static int private_table(void *cookie, const struct giulia_mhi_s1_page *q)
{
    struct giulia_mhi_private *s=cookie;
    if (!s || s->self!=s || !q) return -1;
    /* Descriptors were derived from OWNED stable wired allocations, never
     * supplied by a caller. No lookup against an Android/firmware root. */
    for (uint32_t i=0;i<s->tables;i++)
        if (q->cpu==s->desc[i].cpu && q->pa==s->desc[i].pa) return 0;
    return -1;
}

static int private_tx(void *cookie, uint64_t pa, uint32_t pages)
{
    struct giulia_mhi_private *s=cookie;
    return s && s->self==s && s->phase==GIULIA_MHI_PRIVATE_BUILDING &&
        pages && pages==s->permit_pages && pa==s->permit_pa ? 0 : -1;
}

int giulia_mhi_private_destroy(struct giulia_mhi_private *s)
{
    if (!s) return -1;
    if (!s->self && s->phase==GIULIA_MHI_PRIVATE_EMPTY) return 0;
    if (s->self!=s || s->tables>GIULIA_MHI_S1_MAX_TABLES) return -1;
    s->phase=GIULIA_MHI_PRIVATE_CLEANUP;
    s->permit_pages=0;
    if (s->tree_live) {
        if (giulia_mhi_s1_reset(&s->tree)) return -1;
        s->tree_live=0;
    }
    while (s->tables) {
        if (giulia_mhi_wired_release(&s->store[s->tables-1])) return -1;
        --s->tables;
    }
    __builtin_memset(s,0,sizeof(*s));
    return 0;
}

static int private_map_range(struct giulia_mhi_private *s,
    const struct giulia_mhi_private_range *r, uint32_t bytes)
{
    uint32_t off=0;
    while (off<bytes) {
        uint64_t first=0, next=0;
        if (giulia_mhi_wired_phys4k(r->backing,off,&first)) return -1;
        uint32_t run=1;
        /* Coalesce only actually adjacent translated 4K pages. In particular
         * adjacent CPU addresses across 16K VM page boundaries need not be
         * adjacent PA. A final partial slice is dedicated zero-padded RAM. */
        while (off+run*GIULIA_MHI_S1_PAGE<bytes) {
            if (giulia_mhi_wired_phys4k(r->backing,off+run*GIULIA_MHI_S1_PAGE,&next))
                return -1;
            uint64_t delta=(uint64_t)run*GIULIA_MHI_S1_PAGE;
            if (first>~(uint64_t)0-delta || next!=first+delta) break;
            ++run;
        }
        s->permit_pa=first; s->permit_pages=run;
        int rc=giulia_mhi_s1_map(&s->tree,r->iova+off,first,run);
        s->permit_pa=0; s->permit_pages=0;
        if (rc) return -1;
        off+=run*GIULIA_MHI_S1_PAGE;
    }
    return 0;
}

int giulia_mhi_private_build(struct giulia_mhi_private *s,
    const struct giulia_mhi_private_range *r, uint32_t count, uint32_t oas)
{
    if (!s || s->self || s->phase!=GIULIA_MHI_PRIVATE_EMPTY || !r ||
        !count || count>GIULIA_MHI_STAGE_SLOTS ||
        !(oas==32 || oas==36 || oas==40 || oas==42 || oas==44 || oas==48)) return -1;
    uint8_t seen[GIULIA_MHI_S1_LEAVES]={0};
    uint32_t lengths[GIULIA_MHI_STAGE_SLOTS];
    uint32_t tables=2;
    for (uint32_t i=0;i<count;i++) {
        void *cpu=0; uint32_t n=0;
        if (giulia_mhi_wired_cpu(r[i].backing,&cpu,&n) || !cpu || !n ||
            n>GIULIA_MHI_WIRED_MAX || (r[i].iova&(GIULIA_MHI_S1_PAGE-1)) ||
            r[i].iova<GIULIA_MHI_S1_FIRST || r[i].iova>GIULIA_MHI_S1_LAST ||
            n-1u>GIULIA_MHI_S1_LAST-r[i].iova) return -1;
        lengths[i]=n;
        uint64_t end=r[i].iova+n-1u;
        /* Starts are page-aligned, so overlapping rounded tails imply
         * overlapping requested byte ranges too. */
        for (uint32_t j=0;j<i;j++)
            if (r[j].backing==r[i].backing ||
                (r[j].iova<=end && r[i].iova<r[j].iova+lengths[j])) return -1;
        uint32_t lo=(uint32_t)((r[i].iova-GIULIA_MHI_S1_FIRST)>>21);
        uint32_t hi=(uint32_t)((end-GIULIA_MHI_S1_FIRST)>>21);
        for (uint32_t j=lo;j<=hi;j++) if (!seen[j]) { seen[j]=1; ++tables; }
    }
    s->self=s; s->phase=GIULIA_MHI_PRIVATE_BUILDING;
    for (uint32_t i=0;i<tables;i++) {
        if (giulia_mhi_wired_alloc(&s->store[i],GIULIA_MHI_S1_PAGE)) goto fail;
        ++s->tables;
        void *cpu=0; uint32_t n=0;
        if (giulia_mhi_wired_cpu(&s->store[i],&cpu,&n) || n!=GIULIA_MHI_S1_PAGE ||
            giulia_mhi_wired_phys4k(&s->store[i],0,&s->desc[i].pa)) goto fail;
        s->desc[i].cpu=cpu;
    }
    const struct giulia_mhi_s1_ops ops={s,private_table,private_tx};
    if (giulia_mhi_s1_init(&s->tree,&ops,s->desc,tables,oas)) goto fail;
    s->tree_live=1;
    for (uint32_t i=0;i<count;i++)
        if (private_map_range(s,r+i,lengths[i])) goto fail;
    s->phase=GIULIA_MHI_PRIVATE_READY;
    return 0;
fail:
    (void)giulia_mhi_private_destroy(s);
    return -1;
}
