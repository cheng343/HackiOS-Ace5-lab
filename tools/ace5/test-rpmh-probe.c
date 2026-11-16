/* SPDX-License-Identifier: BSD-2-Clause */
/* Cloud-only: actual XNU entry + actual RPMh/ARC cores, synthetic peripherals.
 * The model erases RESP_DATA on IRQ clear, so capture order is observable. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../xnu/files/bsd/sys/loader_firmware_quiet.h"
#define GIULIA_RPMH_PROBE_HOST_TEST
#define __unused __attribute__((unused))
#define KERN_SUCCESS 0
#define VM_WIMG_IO 7u
#define VM_WIMG_WCOMB 6u
#define trunc_page(x) ((uintptr_t)(x)&~(uintptr_t)0x3fff)
#define round_page(x) (((uintptr_t)(x)+0x3fff)&~(uintptr_t)0x3fff)
typedef uintptr_t vm_offset_t;
typedef uintptr_t vm_size_t;
typedef int kern_return_t;
typedef int wait_result_t;
typedef void *thread_t;
static _Alignas(0x4000) uint32_t shared_page[0x4000/4],rsc[0x10000/4],db[0x20000/4],gic[0x10000/4];
static uint32_t addresses[23],votes[23],original[3],gic_enabled;
static unsigned maps,unmaps,fail_map,starts,exits,complete,cleaned,time_us,issued,triggers;
static unsigned hold_at,drift_at,bad_reply,sticky_pending,no_ack,drop_mask,mask_writes,unmask_writes,lose_guard;
static volatile struct loader_firmware_quiet *const shared=(void *)((uint8_t *)shared_page+0x100);
static void peer(void);
#define __sync_synchronize() peer()
static void delay(unsigned us) { time_us+=us; assert(time_us<4000000); }
static thread_t current_thread(void) { return (void *)(uintptr_t)1; }
static void thread_deallocate(thread_t t) { assert(t==current_thread()); }
static int kernel_thread_start(void (*fn)(void *,wait_result_t),void *arg,thread_t *out)
{ starts++; *out=current_thread(); fn(arg,0); return 0; }
static int quiet_printf(const char *fmt,...) { (void)fmt; return 0; }
#define printf quiet_printf
#include "../../xnu/files/bsd/dev/giulia_rpmh_probe.c"
#undef printf
#undef __sync_synchronize
static void peer(void)
{
    if (!no_ack) { shared->ack=shared->request; shared->active=shared->request; }
    if (gic[0x184/4]) {
        assert(shared->ack && gic[0x184/4]==GR_IRQ);
        mask_writes++; if (!drop_mask) gic_enabled&=~GR_IRQ;
        gic[0x184/4]=0;
        gic[0x104/4]=gic_enabled;
    }
    if (gic[0x104/4]!=gic_enabled) {
        assert(gic[0x104/4]==GR_IRQ && shared->ack);
        unmask_writes++; gic_enabled|=gic[0x104/4];
    }
    gic[0x104/4]=gic_enabled;
    if (!(rsc[0xd24/4]&(1u<<24))) issued=0;
    if (!issued && (rsc[0xd24/4]&(1u<<24))) {
        unsigned i=0; issued=1; triggers++;
        assert(shared->ack && !(gic_enabled&GR_IRQ));
        assert(rsc[0xd34/4]==0x108 && rsc[0xd3c/4]==original[2]);
        assert(rsc[0xd2c/4]==1 && (rsc[0xd20/4]&1));
        while (i<23 && addresses[i]!=rsc[0xd38/4]) i++;
        assert(i<23);
        rsc[0xd28/4]=0;
        if (triggers!=hold_at) {
            rsc[0xd28/4]=1; rsc[0xd40/4]=0x10100;
            rsc[0xd44/4]=bad_reply ? UINT32_MAX:votes[i]+(drift_at && triggers>=drift_at);
            rsc[0xd04/4]|=1; gic[0x204/4]|=GR_IRQ;
        }
        if (triggers==lose_guard) gic_enabled|=GR_IRQ;
    }
    if (rsc[0xd08/4]) {
        assert(rsc[0xd08/4]==1); rsc[0xd04/4]&=~1u; rsc[0xd08/4]=0;
        rsc[0xd44/4]=0xdeadbeef;
        if (!sticky_pending) gic[0x204/4]&=~GR_IRQ;
    }
    gic[0x104/4]=gic_enabled;
}
vm_offset_t ml_io_map_unmappable(vm_offset_t pa,vm_size_t size,unsigned flags)
{
    unsigned bit; void *base;
    if (pa==0x90000000) { bit=1;base=shared_page;assert(size==0x4000 && flags==VM_WIMG_WCOMB); }
    else if (pa==0x17a20000) { bit=2;base=rsc;assert(size==sizeof(rsc) && flags==VM_WIMG_IO); }
    else if (pa==0x81c60000) { bit=4;base=db;assert(size==sizeof(db) && flags==VM_WIMG_WCOMB); }
    else { assert(pa==0x17100000 && size==sizeof(gic) && flags==VM_WIMG_IO);bit=8;base=gic; }
    if (bit==fail_map) return 0;
    assert(!(maps&bit)); maps|=bit; return (uintptr_t)base;
}
void ml_io_unmap(vm_offset_t va,vm_size_t size)
{
    unsigned bit=va==(uintptr_t)shared_page ? 1:va==(uintptr_t)rsc ? 2:va==(uintptr_t)db ? 4:8;
    assert(size==(bit==1 ? sizeof(shared_page):bit==2 ? sizeof(rsc):bit==4 ? sizeof(db):sizeof(gic)));
    assert((maps&bit) && !(unmaps&bit));unmaps|=bit;
}
kern_return_t thread_terminate(thread_t t) { assert(t==current_thread());exits++;return 0; }
void loader_log_line(const char *s)
{ complete+=strstr(s,"capture complete;")!=0;cleaned+=strstr(s,"cleanup complete;")!=0; }
static void put16(uint8_t *p,unsigned v) { p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8); }
static void put32(uint8_t *p,uint32_t v) { memcpy(p,&v,4); }
static void setup(void)
{
    assert(!giulia_rpmh_bus_owned(&gr.rpmh));memset(&gr,0,sizeof(gr));
    memset(shared_page,0,sizeof(shared_page));memset(rsc,0,sizeof(rsc));memset(db,0,sizeof(db));memset(gic,0,sizeof(gic));
    shared->magic=LOADER_QUIET_MAGIC;shared->version=1;shared->size=sizeof(*shared);
    maps=unmaps=fail_map=starts=exits=complete=cleaned=time_us=issued=triggers=0;
    hold_at=drift_at=bad_reply=sticky_pending=no_ack=drop_mask=mask_writes=unmask_writes=lose_guard=0;
    gic_enabled=GR_IRQ|1;gic[0x104/4]=gic_enabled;gic[0xffe8/4]=0x3b;
    rsc[0]=3u<<16;rsc[0x0c/4]=(8u<<12)|(16u<<27);rsc[0xd00/4]=8;rsc[0xd20/4]=8;
    for (unsigned i=0;i<3;i++) rsc[(0xd28+i*672)/4]=1;
    original[0]=0x1234;original[1]=0x4568;original[2]=0x9876;
    memcpy(&rsc[0xd34/4],original,sizeof(original));
    uint8_t *b=(uint8_t *)db;db[0]=7;db[1]=0xc0330db;
    put16(b+8,3);put16(b+12,512);put16(b+14,2);
    put16(b+24,4);put16(b+26,48);put16(b+28,600);put16(b+30,7);
    for (unsigned i=0;i<9;i++) {
        uint8_t *e=b+144+i*24;memcpy(e,gr_names[i],strlen(gr_names[i]));
        if (i<2) {
            addresses[i]=0x31080+i*16;put32(e+16,addresses[i]);
            put16(e+20,16);put16(e+22,i*16);
            for (unsigned j=0;j<6;j++) put16(b+656+i*16+j*2,j*16);
            votes[i]=i+2;
        } else {
            unsigned n=2+(i-2)*3;put32(e+16,0x40200+(i-2)*16);
            for (unsigned j=0;j<3;j++) { addresses[n+j]=0x40200+(i-2)*16+j*4;votes[n+j]=1000+(i-2)*10+j; }
        }
    }
}
static void run(void) { giulia_rpmh_read_test_start(0x90000100);assert(starts==1 && exits==1); }
static void recovered(void)
{
    if (giulia_rpmh_bus_owned(&gr.rpmh)) assert(!giulia_rpmh_recover(&gr.rpmh));
    assert(gr_irq_release() && gr_request(0));
    gr_unmap(&gr.gic);gr_unmap(&gr.db);gr_unmap(&gr.rsc);gr_unmap(&gr.shared);
}
int main(void)
{
    setup();run();assert(complete && cleaned && gr.reads==46 && triggers==46 && maps==unmaps);
    assert(gic_enabled==(GR_IRQ|1) && mask_writes==1 && unmask_writes==1 && !shared->request);
    assert(!memcmp(original,&rsc[0xd34/4],sizeof(original)) && rsc[0xd20/4]==8 && rsc[0xd00/4]==8);
    for (unsigned i=0;i<23;i++) assert(gr.raw[0][i]==votes[i] && gr.raw[1][i]==votes[i]);
    setup();gic_enabled=gic[0x104/4]=1;run();assert(complete && cleaned && !unmask_writes && gic_enabled==1);
    for (unsigned bit=1;bit<=8;bit<<=1) { setup();fail_map=bit;run();assert(!triggers && maps==unmaps && !mask_writes); }
    setup();db[1]=0;run();assert(!triggers && !mask_writes && maps==unmaps);
    setup();no_ack=1;run();assert(!triggers && !mask_writes && maps==unmaps && !shared->request);
    setup();gic[0x304/4]=GR_IRQ;run();assert(!triggers && !mask_writes && maps==unmaps);
    setup();gic[0x204/4]=GR_IRQ;run();assert(!triggers && !mask_writes && maps==unmaps);
    setup();drop_mask=1;run();assert(!triggers && maps==unmaps && !shared->request);
    setup();drift_at=24;run();assert(gr.reads==46 && !complete && !cleaned && maps==unmaps);
    setup();hold_at=1;run();assert(triggers==1 && !complete && !unmaps && shared->request);
    assert(giulia_rpmh_bus_owned(&gr.rpmh) && gr.irq_dirty && !(gic_enabled&GR_IRQ));
    rsc[0xd28/4]=1;rsc[0xd04/4]=1;rsc[0xd40/4]=0x10100;rsc[0xd44/4]=2;hold_at=0;
    recovered();assert(triggers==1 && maps==unmaps);
    setup();bad_reply=1;run();assert(triggers==1 && !complete && !unmaps && giulia_rpmh_bus_owned(&gr.rpmh));
    bad_reply=0;rsc[0xd44/4]=2;recovered();assert(triggers==1);
    setup();sticky_pending=1;run();assert(!complete && !unmaps && !giulia_rpmh_bus_owned(&gr.rpmh));
    assert(gr.irq_dirty && shared->request && !(gic_enabled&GR_IRQ));
    gic[0x204/4]=0;recovered();
    setup();lose_guard=1;run();assert(triggers==1 && !complete && !unmaps && gr.guard_failed);
    gic_enabled=gic[0x104/4]=1;gr.guard_failed=0;recovered();assert(triggers==1);
    puts("Ace5 RPMh READ XNU: real 46-query capture, IRQ37 isolation, cleanup and retained faults passed");
    return 0;
}
