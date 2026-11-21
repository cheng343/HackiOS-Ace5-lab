/* SPDX-License-Identifier: BSD-2-Clause */
/* Cloud harness: run production private builder and S1 walker against wired
 * backing with deliberately noncontiguous physical pages. The independent
 * wired test replaces the kernel VM calls rather than this interface. */
#include "giulia_mhi_private.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define P 4096u
static _Alignas(4096) uint8_t ram[10][16384];
static struct giulia_mhi_wired *regs[10];
static uint64_t addr[10][4];
static unsigned attempts, fail_alloc, fail_free, nalloc;
static int slot(const struct giulia_mhi_wired *b) {
    for (int j=0;j<10;j++) if (b && regs[j]==b) return j;
    return -1;
}
int giulia_mhi_wired_alloc(struct giulia_mhi_wired *b,uint32_t n) {
    if (++attempts==fail_alloc || !b || !n || n>sizeof ram[0]) return -1;
    for (int j=0;j<10;j++) if (!regs[j]) {
        regs[j]=b; b->bytes=n; b->self=b; nalloc++;
        memset(ram[j],0,sizeof ram[j]);
        for (unsigned i=0;i<4;i++)
            addr[j][i]=0x100000000ULL+(uint64_t)j*0x500000+i*0x3000;
        return 0;
    }
    return -1;
}
int giulia_mhi_wired_cpu(const struct giulia_mhi_wired *b,void **out,uint32_t *n) {
    int j=slot(b); if (j<0) return -1;
    *out=ram[j]; *n=b->bytes; return 0;
}
int giulia_mhi_wired_phys4k(const struct giulia_mhi_wired *b,uint32_t off,uint64_t *out) {
    int j=slot(b);
    if (j<0 || off>=b->bytes || off%P) return -1;
    *out=addr[j][off/P]; return 0;
}
int giulia_mhi_wired_release(struct giulia_mhi_wired *b) {
    int j=slot(b); if (j<0) return -1;
    if (fail_free) { --fail_free; return -1; }
    regs[j]=NULL; --nalloc; memset(b,0,sizeof *b); return 0;
}
static uint64_t entry(const uint8_t *p,unsigned i) {
    uint64_t n=0; for (unsigned j=0;j<8;j++) n|=(uint64_t)p[8*i+j]<<(8*j);
    return n;
}
static uint8_t *step(uint64_t desc) {
    assert((desc&4095u)==3);
    for (int i=0;i<10;i++) if (regs[i] && addr[i][0]==(desc&~4095ULL)) return ram[i];
    abort();
}
static uint64_t walk(const struct giulia_mhi_private *s,uint64_t va) {
    const uint8_t *l2=step(entry(s->desc[0].cpu,(unsigned)(va>>30)));
    uint64_t m=entry(l2,(va>>21)&511); if (!m) return 0;
    return entry(step(m),(va>>12)&511);
}
static struct giulia_mhi_private s;
static struct giulia_mhi_wired a,b;
int main(void) {
    const uint64_t first=GIULIA_MHI_S1_FIRST+0x1ff000, flags=0x0060000000000ec7ULL;
    assert(!giulia_mhi_wired_alloc(&a,2*P+53));
    assert(!giulia_mhi_wired_alloc(&b,16));
    /* first segment crosses a leaf-table boundary; last is in a disjoint 2M region */
    struct giulia_mhi_private_range r[]={{&a,first},{&b,GIULIA_MHI_S1_LAST+1-P}};
    addr[0][1]=addr[0][0]+P;
    ram[0][0]=0x4b; ram[1][15]=0xa3;
    assert(!giulia_mhi_private_build(&s,r,2,40));
    assert(s.phase==GIULIA_MHI_PRIVATE_READY && s.tables==5 && nalloc==7);
    for (unsigned i=0;i<3;i++) assert(walk(&s,first+i*P)==(addr[0][i]|flags));
    assert(walk(&s,r[1].iova)==(addr[1][0]|flags));
    assert(!walk(&s,first-P) && !walk(&s,first+3*P));
    assert(giulia_mhi_private_build(&s,r,2,40));
    fail_free=1; assert(giulia_mhi_private_destroy(&s) && nalloc==7 && !s.tree_live);
    assert(!giulia_mhi_private_destroy(&s) && !giulia_mhi_private_destroy(&s) && nalloc==2);
    for (unsigned i=1;i<=5;i++) {
        fail_alloc=attempts+i; assert(giulia_mhi_private_build(&s,r,2,40));
        assert(nalloc==2 && s.phase==GIULIA_MHI_PRIVATE_EMPTY);
    }
    fail_alloc=0; addr[1][0]=addr[0][2];
    assert(giulia_mhi_private_build(&s,r,2,40) && nalloc==2 && !s.self);
    addr[1][0]=1ULL<<40; assert(giulia_mhi_private_build(&s,r,2,40) && nalloc==2);
    addr[1][0]=0x100500000ULL; r[1].iova=r[0].iova+2*P;
    unsigned prev=attempts;
    assert(giulia_mhi_private_build(&s,r,2,40) && attempts==prev);
    r[1].iova=GIULIA_MHI_S1_LAST+1;
    assert(giulia_mhi_private_build(&s,r,2,40) && attempts==prev);
    r[1].iova=first+3*P; r[1].backing=&a;
    assert(giulia_mhi_private_build(&s,r,2,40) && attempts==prev);
    assert(ram[0][0]==0x4b && ram[1][15]==0xa3 && nalloc==2);
    assert(!giulia_mhi_wired_release(&b) && !giulia_mhi_wired_release(&a) && !nalloc);
    puts("KIWI private tree: wired TX scatter, sparse 4K walk, permissions and rollback passed");
    return 0;
}
