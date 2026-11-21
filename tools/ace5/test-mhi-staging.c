/* SPDX-License-Identifier: BSD-2-Clause */
/* Synthetic, offline WCN7850 staging contract. No device, MMIO or real IOVA. */
#ifdef NDEBUG
#error "This fixture requires assertions"
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "giulia_mhi_staging.h"

enum { CHUNK=524288, TAIL=73, N=2*CHUNK+TAIL, NS=5, GUARD=32 };
#define BASE UINT64_C(0x98000000)
#define LAST UINT64_C(0xafffffff)
#define EXPECT_SID UINT32_C(0x1401)
#define EXPECT_PHASE(x) (st.phase == GIULIA_MHI_STAGE_##x)
static uint8_t firmware[N];
struct slot {
    uint8_t *allocation, *cpu;
    uint32_t cap, synced;
    uint64_t iova;
    int live;
    struct giulia_mhi_stage_buffer published; /* independent of our backing ledger */
};
enum injection { NORMAL, BAD_POOL, OVERLAP_IOVA, OVERLAP_CPU, HOST_AS_IOVA,
    NULL_HANDLE, DUP_HANDLE, ERROR_WITH_HANDLE };
struct machine {
    struct slot slots[NS];
    unsigned allocations, alloc_calls, sync_calls, free_calls, pins, unpins, pin_released, quiet_checks;
    unsigned fail_alloc, fail_sync, lose_during_sync, free_fails, fail_unpin;
    enum injection inject;
    int refuse_pin, failed_free_idx, live, quiet, in_flight, token, pin_held, freed[NS], trace[3*NS];
};
static struct machine m;
static struct giulia_mhi_stage_state st;

static void verify_owner(void *c, void *owner)
{
    assert(c==&m && owner==&m.token && m.pin_held);
}
static int pin_owner(void *c,uint32_t sid,uint64_t first,uint64_t last,void **owner)
{
    assert(c==&m && sid==EXPECT_SID && first==BASE && last==LAST && owner && !m.pin_held);
    m.pins++;
    if (m.refuse_pin) return -1; /* no token is transferred on refusal */
    m.pin_held=1; *owner=&m.token;
    return 0;
}
static int live_owner(void *c,void *owner)
{
    verify_owner(c,owner);
    return m.live ? 0 : -1;
}
static int owns_buffer(void *c,void *owner,const struct giulia_mhi_stage_buffer *buf)
{
    verify_owner(c,owner);
    assert(m.live);
    for (unsigned i=0;i<m.allocations;i++) {
        struct slot *s=&m.slots[i];
        const struct giulia_mhi_stage_buffer *published=&s->published;
        if (s->live && buf->handle==published->handle && buf->cpu==published->cpu &&
            buf->iova==published->iova && buf->bytes==published->bytes)
            return 0;
    }
    return -1;
}
static int allocate(void *c,void *owner,uint32_t bytes,uint32_t align,struct giulia_mhi_stage_buffer *out)
{
    verify_owner(c,owner);
    assert(m.live && align && !(align & (align-1u)) && bytes && out && m.allocations<NS);
    m.alloc_calls++;
    if (m.fail_alloc==m.alloc_calls) return -1;
    unsigned i=m.allocations++;
    struct slot *s=&m.slots[i];
    s->cap=bytes;
    s->iova=BASE+UINT64_C(0x100000)*(2u+i*3u);
    assert(!(s->iova & (align-1u)));
    s->allocation=malloc((size_t)s->cap+2*GUARD); assert(s->allocation);
    memset(s->allocation,0xa5,(size_t)s->cap+2*GUARD);
    s->cpu=s->allocation+GUARD; s->live=1;
    assert((uint64_t)(uintptr_t)s->cpu!=s->iova);
    s->published=(struct giulia_mhi_stage_buffer){.handle=s,.cpu=s->cpu,.iova=s->iova,.bytes=s->cap};
    /* Inject only in the last allocation. All earlier valid buffers must
     * still be untouched when this descriptor is rejected. */
    if (i==NS-1) switch (m.inject) {
    case BAD_POOL:        s->published.iova=LAST-7; break; /* aligned, end outside pool */
    case OVERLAP_IOVA:    s->published.iova=m.slots[0].iova+8; break;
    case OVERLAP_CPU:     s->published.cpu=m.slots[0].cpu+8; break;
    case HOST_AS_IOVA:    s->published.iova=(uint64_t)(uintptr_t)s->cpu; break;
    case NULL_HANDLE:     s->published.handle=NULL; break;
    case DUP_HANDLE:      s->published.handle=&m.slots[0]; break;
    case ERROR_WITH_HANDLE: case NORMAL: break;
    }
    *out=s->published;
    return i==NS-1 && m.inject==ERROR_WITH_HANDLE ? -1 : 0;
}
static uint64_t get_le64(const uint8_t *p)
{
    uint64_t value=0;
    for (unsigned j=0;j<8;j++) value|=(uint64_t)p[j]<<(8u*j);
    return value;
}
static void check_guards(struct slot *s)
{
    for (unsigned j=0;j<GUARD;j++)
        assert(s->allocation[j]==0xa5 && s->cpu[s->cap+j]==0xa5);
}
static void check_untouched(const struct slot *s)
{
    /* physical mock backing, not the possibly aliased returned CPU pointer */
    for (size_t j=0;j<(size_t)s->cap+2*GUARD;j++) assert(s->allocation[j]==0xa5);
}
static int sync_buffer(void *c,void *owner,const struct giulia_mhi_stage_buffer *buf,uint32_t used)
{
    verify_owner(c,owner);
    assert(m.inject==NORMAL && !owns_buffer(c,owner,buf) && used && used<=buf->bytes);
    unsigned i=(unsigned)((struct slot *)buf->handle-m.slots);
    assert(i<NS && i==m.sync_calls && !m.slots[i].synced);
    m.sync_calls++;
    struct slot *s=&m.slots[i];
    check_guards(s);
    /* Verify every byte at publish/sync time, not only after prepare returns.
     * BHIE segment 0 must duplicate the BHI prefix; segment 1 must not. */
    if (i<4) {
        static const uint32_t offsets[]={0,0,CHUNK,2*CHUNK};
        assert(used==(i==3 ? TAIL : CHUNK));
        assert(!memcmp(s->cpu,firmware+offsets[i],used));
    } else {
        assert(used==48);
        for (unsigned j=0;j<3;j++) {
            const uint8_t *vec=s->cpu+16u*j;
            assert(get_le64(vec)==m.slots[j+1].iova &&
                get_le64(vec)!=(uint64_t)(uintptr_t)m.slots[j+1].cpu);
            assert(get_le64(vec+8)==(j==2 ? TAIL : CHUNK));
            assert(m.slots[j+1].synced);
        }
    }
    if (m.fail_sync==m.sync_calls) {
        if (m.lose_during_sync) m.live=0;
        return -1;
    }
    s->synced=used;
    return 0;
}
static int release_buffer(void *c,void *owner,void *handle)
{
    verify_owner(c,owner);
    assert(m.live && (!m.in_flight || m.quiet) && m.free_calls<3*NS);
    unsigned i=(unsigned)((struct slot *)handle-m.slots); assert(i<m.allocations);
    struct slot *s=&m.slots[i]; assert(s->live && !m.freed[i]);
    m.trace[m.free_calls++]=(int)i; check_guards(s);
    if (m.inject!=NORMAL) check_untouched(s);
    if ((int)i==m.failed_free_idx && m.free_fails) { m.free_fails--; return -1; }
    free(s->allocation); s->allocation=s->cpu=NULL; s->live=0; m.freed[i]++;
    return 0;
}
static int quiesced(void *c,void *owner)
{
    verify_owner(c,owner); assert(m.live); m.quiet_checks++;
    return m.quiet ? 0 : -1;
}
static int unpin_owner(void *c,void *owner)
{
    verify_owner(c,owner); assert(m.live && (!m.in_flight || m.quiet));
    for(unsigned i=0;i<m.allocations;i++) assert(!m.slots[i].live);
    m.unpins++;
    if (m.fail_unpin) { m.fail_unpin--; return -1; }
    m.pin_held=0; m.pin_released++;
    return 0;
}
static const struct giulia_mhi_stage_ops ops={
    .cookie=&m,.pin=pin_owner,.live=live_owner,.owns=owns_buffer,.alloc=allocate,
    .sync=sync_buffer,.free=release_buffer,.quiesced=quiesced,.unpin=unpin_owner
};
static void fresh(void)
{
    assert(!m.pin_held);
    for(unsigned i=0;i<m.allocations;i++) assert(!m.slots[i].live);
    memset(&m,0,sizeof m); memset(&st,0,sizeof st);
    m.live=m.quiet=1; m.failed_free_idx=-1;
}
static void clean_reverse(void)
{
    assert(EXPECT_PHASE(EMPTY) && !st.count && !m.pin_held && m.pin_released==1);
    assert(m.free_calls==m.allocations);
    for(unsigned i=0;i<m.allocations;i++)
        assert(m.trace[i]==(int)(m.allocations-1-i) && m.freed[i]==1);
}
static int prepare_image(void)
{
    return giulia_mhi_stage_prepare(&st,&ops,firmware,sizeof firmware);
}
static void round_trip(void)
{
    fresh();
    assert(!prepare_image() && EXPECT_PHASE(READY) && m.allocations==NS && m.sync_calls==NS);
    assert(!m.free_calls && m.pin_held);
    struct giulia_mhi_stage_view view={0},duplicate={0};
    assert(!giulia_mhi_stage_submit(&st,&view) && EXPECT_PHASE(FLIGHT));
    assert(view.bhi_iova==m.slots[0].iova && view.bhi_bytes==CHUNK);
    assert(view.bhie_iova==m.slots[4].iova && view.bhie_bytes==48);
    m.in_flight=1; m.quiet=0;
    assert(giulia_mhi_stage_submit(&st,&duplicate)!=0);
    for(unsigned i=0;i<2;i++) {
        assert(giulia_mhi_stage_release(&st)!=0 && !m.free_calls && !m.unpins && m.pin_held);
    }
    assert(m.quiet_checks);
    m.quiet=1; assert(!giulia_mhi_stage_release(&st)); clean_reverse();
}
static void partial_failures(void)
{
    for(unsigned failure=1;failure<=NS;failure++) {
        fresh(); m.fail_alloc=failure;
        assert(prepare_image()!=0 && m.alloc_calls==failure && m.allocations==failure-1);
        clean_reverse();
    }
    for(unsigned failure=1;failure<=NS;failure++) {
        fresh(); m.fail_sync=failure;
        assert(prepare_image()!=0 && m.sync_calls==failure && m.allocations>=failure);
        clean_reverse();
    }
}
static void retained_resources(void)
{
    fresh(); m.fail_sync=3; m.lose_during_sync=1;
    assert(prepare_image()!=0 && EXPECT_PHASE(RETAINED));
    assert(!m.free_calls && !m.unpins && m.pin_held && m.allocations>=3);
    assert(giulia_mhi_stage_release(&st)!=0 && !m.free_calls && !m.unpins);
    m.live=1; assert(!giulia_mhi_stage_release(&st)); clean_reverse();

    fresh(); assert(!prepare_image()); m.failed_free_idx=2; m.free_fails=1;
    assert(giulia_mhi_stage_release(&st)!=0 && EXPECT_PHASE(RETAINED));
    assert(m.free_calls==3 && m.trace[0]==4 && m.trace[1]==3 && m.trace[2]==2);
    assert(!m.unpins && m.freed[4] && m.freed[3] && !m.freed[2]);
    assert(!giulia_mhi_stage_release(&st) && EXPECT_PHASE(EMPTY));
    assert(m.free_calls==6 && m.trace[3]==2 && m.trace[4]==1 && m.trace[5]==0);
    assert(m.pin_released==1);

    fresh(); assert(!prepare_image()); m.fail_unpin=1;
    assert(giulia_mhi_stage_release(&st)!=0 && EXPECT_PHASE(RETAINED) && m.pin_held);
    assert(m.free_calls==NS && m.unpins==1);
    assert(!giulia_mhi_stage_release(&st) && m.unpins==2); clean_reverse();
}
static void invalid_descriptors(void)
{
    fresh(); m.refuse_pin=1;
    assert(prepare_image()==GIULIA_MHI_STAGE_DOMAIN && EXPECT_PHASE(EMPTY));
    assert(m.pins==1 && !m.pin_held && !m.alloc_calls && !m.sync_calls && !m.unpins);
    const enum injection bad_maps[]={BAD_POOL,OVERLAP_IOVA,OVERLAP_CPU,HOST_AS_IOVA};
    for (unsigned k=0;k<sizeof bad_maps/sizeof *bad_maps;k++) {
        fresh(); m.inject=bad_maps[k];
        assert(prepare_image()==GIULIA_MHI_STAGE_MAPPING);
        assert(m.allocations==NS && !m.sync_calls); clean_reverse();
    }
    const enum injection ambiguous[]={NULL_HANDLE,DUP_HANDLE,ERROR_WITH_HANDLE};
    for (unsigned k=0;k<sizeof ambiguous/sizeof *ambiguous;k++) {
        fresh(); m.inject=ambiguous[k];
        assert(prepare_image()==GIULIA_MHI_STAGE_PROVIDER && EXPECT_PHASE(POISONED));
        assert(st.count==NS && m.allocations==NS && !m.sync_calls && m.pin_held);
        struct giulia_mhi_stage_view view={0};
        assert(giulia_mhi_stage_submit(&st,&view)!=0);
        assert(giulia_mhi_stage_release(&st)==GIULIA_MHI_STAGE_PROVIDER);
        assert(giulia_mhi_stage_release(&st)==GIULIA_MHI_STAGE_PROVIDER);
        assert(prepare_image()!=0 && EXPECT_PHASE(POISONED));
        assert(!m.free_calls && !m.unpins && m.pins==1 && m.allocations==NS);
        /* Out-of-band mock owner destroys its own allocation ledger at case
         * end. This does not call or authorize any staging cleanup. */
        for (unsigned i=0;i<m.allocations;i++) {
            struct slot *s=&m.slots[i];
            assert(s->live); check_untouched(s);
            free(s->allocation); s->allocation=s->cpu=NULL; s->live=0;
        }
        m.pin_held=0;
    }
}
int main(void)
{
    for (size_t i=0;i<sizeof firmware;i++) firmware[i]=(uint8_t)(13u*i^(i>>8)^((i>>19)*91u));
    assert(memcmp(firmware,firmware+CHUNK,CHUNK)!=0);
    round_trip();partial_failures();retained_resources();invalid_descriptors();fresh();
    puts("PASS: KIWI staging checks actual prefix/full image bytes and IOVA vectors; owner loss, flight and reverse retry retain buffers");
    return 0;
}
