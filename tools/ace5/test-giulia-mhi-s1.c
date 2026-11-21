/* SPDX-License-Identifier: BSD-2-Clause */
/* Synthetic, independent inactive-table walk. Does not allocate a live
 * domain or assume builder metadata/identity mappings. */
#ifdef NDEBUG
#error "assertions required"
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "giulia_mhi_s1.h"

enum { P=4096, K=5 };
#define BASE 0x98000000ULL
#define LAST_PAGE 0xaffff000ULL
#define A 0x84000000ULL
#define B 0x86000000ULL
#define TX 0x0060000000000ec7ULL
#define ADDRESS 0x0000fffffffff000ULL
static _Alignas(4096) uint8_t physical[K][P];
static uint8_t before[K][P];
static struct giulia_mhi_s1 s, old;
static struct giulia_mhi_s1_page offer[K];
/* Noncontiguous, out-of-order table backing. */
static const uint64_t table_pa[K]={0x81223000,0x82001000,0x817e3000,0x80554000,0x81022000};
static int denied, tx_allowed;
static unsigned table_checks,tx_checks;
static int owns_table(void *cookie,const struct giulia_mhi_s1_page *pg)
{
    assert(cookie==&s);
    table_checks++;
    for (int i=0;i<K;i++)
        if (pg->pa==table_pa[i] && pg->cpu==physical[i] && i!=denied) return 0;
    return -1;
}
static int owns_tx(void *cookie,uint64_t pa,uint32_t np)
{
    (void)pa; assert(cookie==&s && np);
    tx_checks++; return tx_allowed ? 0 : -1;
}
static const struct giulia_mhi_s1_ops ops={&s,owns_table,owns_tx};
static void fresh(void)
{
    memset(&s,0,sizeof(s)); memset(physical,0,sizeof(physical));
    for(int i=0;i<K;i++) offer[i]=(struct giulia_mhi_s1_page){physical[i],table_pa[i]};
    denied=-1; tx_allowed=1; table_checks=tx_checks=0;
}
static void snap(void) { memcpy(before,physical,sizeof(before)); memcpy(&old,&s,sizeof(old)); }
static void unchanged(void)
{
    assert(!memcmp(before,physical,sizeof(before)) && !memcmp(&old,&s,sizeof(s)));
}
static uint64_t get64(const uint8_t *page,uint32_t index)
{
    uint64_t v=0;
    for(int b=7;b>=0;b--) v=v*256u+page[index*8u+(unsigned)b];
    return v;
}
static const uint8_t *translate_table(uint64_t descriptor)
{
    assert((descriptor&~ADDRESS)==3u);
    for (int i=0;i<K;i++) if ((descriptor&ADDRESS)==table_pa[i]) return physical[i];
    assert(!"table points outside pool"); return NULL;
}
static uint64_t leaf_for(uint64_t iova)
{
    uint64_t r=get64(physical[0],(uint32_t)((iova>>30)&3u));
    if (!r) return 0;
    uint64_t m=get64(translate_table(r),(uint32_t)((iova>>21)&511u));
    if (!m) return 0;
    return get64(translate_table(m),(uint32_t)((iova>>12)&511u));
}
static void test_tree(void)
{
    fresh();
    assert(!giulia_mhi_s1_init(&s,&ops,offer,K,36));
    assert(get64(physical[0],2)==(table_pa[1]|3u));
    assert(!get64(physical[0],1) && !get64(physical[0],3));
    /* Straddle L2 entries 192/193. Validate raw descriptor bytes/permissions. */
    uint64_t start=BASE+0x1fe000;
    unsigned previous=tx_checks;
    assert(!giulia_mhi_s1_map(&s,start,A,4)); assert(tx_checks==previous+1);
    for (unsigned i=0;i<4;i++) assert(leaf_for(start+P*i)==((A+P*i)|TX));
    assert(!leaf_for(start-P) && !leaf_for(start+4*P));
    assert(!get64(physical[1],191) && !get64(physical[1],194));
    assert(get64(physical[1],192)==(table_pa[2]|3u));
    assert(get64(physical[1],193)==(table_pa[3]|3u));
    assert(!giulia_mhi_s1_map(&s,LAST_PAGE,0xffffff000ULL,1));
    assert(leaf_for(LAST_PAGE)==(0xffffff000ULL|TX));
    snap(); assert(giulia_mhi_s1_map(&s,LAST_PAGE,0x1000000000ULL,1)!=0); unchanged();
    snap(); assert(giulia_mhi_s1_map(&s,LAST_PAGE,B,2)!=0); unchanged();
    snap(); assert(giulia_mhi_s1_map(&s,BASE-4096,B,1)!=0); unchanged();
    snap(); assert(giulia_mhi_s1_map(&s,BASE+4093,B,1)!=0); unchanged();
    snap(); assert(giulia_mhi_s1_map(&s,BASE,B+1,1)!=0); unchanged();
    snap(); assert(giulia_mhi_s1_map(&s,BASE,B,0)!=0); unchanged();
    snap(); assert(giulia_mhi_s1_map(&s,BASE,B,0xffffffffu)!=0); unchanged();
    /* Wrong PA at the second half must NOT remove the first half. */
    assert(!giulia_mhi_s1_unmap(&s,start+2*P,A+2*P,2));
    assert(!giulia_mhi_s1_map(&s,start+2*P,B,2));
    snap(); assert(giulia_mhi_s1_unmap(&s,start,A,4)!=0); unchanged();
    assert(!giulia_mhi_s1_unmap(&s,start,A,2)); assert(!leaf_for(start));
    assert(leaf_for(start+2*P)==(B|TX));
    /* Aliases into a different leaf cannot be introduced. */
    snap(); assert(giulia_mhi_s1_map(&s,BASE,B+P,1)!=0); unchanged();
    snap(); assert(giulia_mhi_s1_map(&s,BASE,table_pa[0],1)!=0); unchanged();
    unsigned calls=tx_checks; tx_allowed=0;
    snap(); denied=4; assert(giulia_mhi_s1_reset(&s)!=0); unchanged();
    denied=-1; assert(!giulia_mhi_s1_reset(&s)); assert(tx_checks==calls);
    for(int i=0;i<K;i++) for(int j=0;j<P;j++) assert(!physical[i][j]);
    assert(!giulia_mhi_s1_reset(&s));
}
static void test_atomic_map(void)
{
    fresh(); assert(!giulia_mhi_s1_init(&s,&ops,offer,4,32));
    uint64_t left=BASE+0x1ff000;
    assert(!giulia_mhi_s1_map(&s,left+P,B,1)); /* last entry conflicts */
    snap(); assert(giulia_mhi_s1_map(&s,left,A,2)!=0); unchanged();
    assert(!get64(physical[1],192));
    snap(); tx_allowed=0; assert(giulia_mhi_s1_map(&s,left,A,1)!=0); unchanged();
    tx_allowed=1; snap(); denied=2; assert(giulia_mhi_s1_map(&s,left,A,1)!=0); unchanged();
    denied=-1; snap(); assert(giulia_mhi_s1_map(&s,left,table_pa[3],1)!=0); unchanged();
    /* One unused page left; distinct fresh branches needed, insufficient capacity. */
    snap(); assert(giulia_mhi_s1_map(&s,BASE+0x5ff000,A,2)==GIULIA_MHI_S1_CAPACITY); unchanged();
    assert(!giulia_mhi_s1_map(&s,left,A,1)); assert(leaf_for(left)==(A|TX));
    snap(); assert(giulia_mhi_s1_map(&s,left+P-1,B,2)!=0); unchanged();
    assert(!giulia_mhi_s1_reset(&s));
}
static void test_pool_admission(void)
{
    fresh(); snap(); assert(giulia_mhi_s1_init(&s,&ops,offer,2,36)!=0); unchanged();
    snap(); assert(giulia_mhi_s1_init(&s,&ops,offer,K,37)!=0); unchanged();
    offer[K-1].pa=table_pa[3]; snap(); assert(giulia_mhi_s1_init(&s,&ops,offer,K,36)!=0); unchanged();
    fresh(); offer[K-1].cpu=physical[3]; snap(); assert(giulia_mhi_s1_init(&s,&ops,offer,K,36)!=0); unchanged();
    fresh(); offer[K-1].pa+=4; snap(); assert(giulia_mhi_s1_init(&s,&ops,offer,K,36)!=0); unchanged();
    fresh(); offer[K-1].pa=0x1000000000ULL; snap(); assert(giulia_mhi_s1_init(&s,&ops,offer,K,36)!=0); unchanged();
    fresh(); physical[K-1][4095]=0x33; snap(); assert(giulia_mhi_s1_init(&s,&ops,offer,K,36)!=0); unchanged();
    fresh(); denied=K-1; snap(); assert(giulia_mhi_s1_init(&s,&ops,offer,K,36)!=0); unchanged();
    fresh(); assert(!giulia_mhi_s1_init(&s,&ops,offer,K,36));
    snap(); assert(giulia_mhi_s1_init(&s,&ops,offer,K,36)!=0); unchanged();
    assert(!giulia_mhi_s1_reset(&s));
}
int main(void)
{
    test_tree();test_atomic_map();test_pool_admission();
    puts("PASS: detached KIWI S1 raw three-level walk, RO/XN pages, no PA aliases, atomic map/unmap and owner-gated reset");
    return 0;
}
