/* SPDX-License-Identifier: BSD-2-Clause */
/* Cloud-only: production XNU adapter + CRM core, simulated loader and MMIO. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../xnu/files/bsd/sys/loader_firmware_quiet.h"
#define GIULIA_CRM_XNU_HOST_TEST
#define __unused __attribute__((unused))
#define KERN_SUCCESS 0
#define kSuccess 0
#define HACKIOS_PLATFORM_GIULIA 2
#define VM_WIMG_IO 7u
#define VM_WIMG_WCOMB 6u
#define PAGE_SIZE ((uintptr_t)0x4000)
#define trunc_page(x) ((uintptr_t)(x)&~(PAGE_SIZE-1))
#define round_page(x) (((uintptr_t)(x)+PAGE_SIZE-1)&~(PAGE_SIZE-1))
typedef uintptr_t vm_offset_t;
typedef uintptr_t vm_size_t;
typedef int kern_return_t;
typedef int wait_result_t;
typedef void *thread_t;
typedef int DTEntry;
static unsigned enabled,board,cpu,cpus,seconds,other,missing_property,property_size;
static unsigned creates,exits,deallocs,maps,unmaps,fail_map,no_quiet_ack,no_channel_ack;
static unsigned complete,requests,time_us,thread_error;
static _Alignas(0x4000) uint32_t shared_page[0x4000/4],gcc[0x70000/4],crm_page[0x4000/4];
static uint32_t *const crm=crm_page+0x1000/4;
static uint64_t shared_pa=0x90000100;
static volatile struct loader_firmware_quiet *const shared=
    (volatile struct loader_firmware_quiet *)((uint8_t *)shared_page+0x100);
static struct { unsigned num_cpus; } topology;
static void peer_barrier(void);
#define __sync_synchronize() peer_barrier()
#define ml_get_topology_info() (&topology)
static void delay(unsigned us) { assert(us==100); time_us+=us; assert(time_us<3000000); }
static int hackios_platform(void) { return (int)board; }
static int PE_parse_boot_argn(const char *name,void *out,unsigned size)
{
    unsigned value=0; assert(size==4);
    if (!strcmp(name,"ace5_crm_channel_test")) value=enabled;
    if (!strcmp(name,"cpus")) value=cpus;
    if (!strcmp(name,"bootcpu")) value=cpu;
    if (!strcmp(name,"loader_return_after")) value=seconds;
    if (!strcmp(name,"ace5_aop_channel_test")) value=other;
    *(unsigned *)out=value; return value!=0;
}
static int SecureDTLookupEntry(void *root,const char *path,DTEntry *e)
{ (void)root; assert(!strcmp(path,"/chosen")); *e=1; return missing_property ? -1 : 0; }
static int SecureDTGetProperty(DTEntry e,const char *name,const void **out,unsigned *size)
{ assert(e==1 && !strcmp(name,"loader-firmware-quiet")); *out=&shared_pa; *size=property_size; return 0; }
static thread_t current_thread(void) { return (void *)(uintptr_t)1; }
static void thread_deallocate(thread_t t) { assert(t==current_thread()); deallocs++; }
static int kernel_thread_start(void (*fn)(void *,wait_result_t),void *arg,thread_t *out)
{ creates++; if (thread_error) return -1; *out=current_thread(); fn(arg,0); return 0; }
static int quiet_printf(const char *format,...) { (void)format; return 0; }
#define printf quiet_printf
#include "../../xnu/files/bsd/dev/giulia_crm_xnu.c"
#undef printf
#undef __sync_synchronize
static void peer_barrier(void)
{
    if (!no_quiet_ack) { shared->ack=shared->request; shared->active=!!shared->request; }
    if (crm[0x1020/4]) {
        assert(crm[0x1020/4]==1 && shared->request==1 && shared->ack==1 && shared->active==1);
        if (!requests) requests++;
        if (!no_channel_ack) crm[0x1000/4]=1;
    }
}
vm_offset_t ml_io_map_unmappable(vm_offset_t pa,vm_size_t size,unsigned flags)
{
    unsigned bit; void *base;
    if (pa==0x90000000) { bit=1; base=shared_page; assert(size==0x4000 && flags==VM_WIMG_WCOMB); }
    else if (pa==0x100000) { bit=2; base=gcc; assert(size==0x70000 && flags==VM_WIMG_IO); }
    else { assert(pa==0x1d00000 && size==0x4000 && flags==VM_WIMG_IO); bit=4; base=crm_page; }
    if (bit==fail_map) return 0;
    assert(!(maps&bit)); maps|=bit; return (uintptr_t)base;
}
void ml_io_unmap(vm_offset_t base,vm_size_t size)
{
    unsigned bit;
    if (base==(uintptr_t)shared_page) { bit=1; assert(size==0x4000); }
    else if (base==(uintptr_t)gcc) { bit=2; assert(size==0x70000); }
    else { assert(base==(uintptr_t)crm_page && size==0x4000); bit=4; }
    assert((maps&bit) && !(unmaps&bit)); unmaps|=bit;
}
kern_return_t thread_terminate(thread_t t) { assert(t==current_thread()); exits++; return 0; }
void loader_log_line(const char *line) { if (strstr(line,"ace5-crm: channel test complete;")) complete++; }
static void setup(void)
{
    memset(&gc,0,sizeof(gc)); gc_started=0;
    memset(shared_page,0,sizeof(shared_page)); memset(gcc,0,sizeof(gcc)); memset(crm_page,0,sizeof(crm_page));
    shared->magic=LOADER_QUIET_MAGIC; shared->version=LOADER_QUIET_VERSION; shared->size=sizeof(*shared);
    gcc[0x6b004/4]=gcc[0x6c000/4]=1; gcc[0x6b008/4]=gcc[0x6c004/4]=0x8000;
    gcc[0x5214c/4]=9;
    crm[0]=0x10000; crm[1]=0x1228225; crm[2]=0x40108128; crm[3]=1;
    board=2; enabled=cpu=cpus=topology.num_cpus=1; seconds=60; property_size=8;
    other=missing_property=creates=exits=deallocs=maps=unmaps=fail_map=0;
    no_quiet_ack=no_channel_ack=complete=requests=time_us=thread_error=0;
}
static void no_start(void) { giulia_crm_test_init(); assert(!creates && !maps && !requests); }
static void rejected(void)
{
    giulia_crm_test_init();
    assert(!requests && !complete && maps==unmaps && exits==1);
    assert(!shared->request && !shared->ack && !shared->active);
}
static void admission(void)
{
    setup(); enabled=0; no_start();
    setup(); enabled=2; no_start();
    setup(); board=1; no_start();
    setup(); cpu=0; no_start();
    setup(); cpus=2; no_start();
    setup(); topology.num_cpus=2; no_start();
    setup(); seconds=0; no_start();
    setup(); seconds=121; no_start();
    setup(); other=1; no_start();
    setup(); missing_property=1; no_start();
    setup(); property_size=4; no_start();
    setup(); thread_error=1; giulia_crm_test_init(); assert(creates==1 && !maps);
    for (unsigned bit=1;bit<=4;bit<<=1) { setup(); fail_map=bit; rejected(); }
    setup(); shared->version=2; rejected();
    setup(); no_quiet_ack=1; rejected(); assert(time_us==1000000);
    setup(); gcc[0x6b008/4]=0x10000; rejected();
    setup(); gcc[0x6c000/4]=3; rejected();
    setup(); gcc[0x52008/4]=1; rejected();
    setup(); gcc[0x5214c/4]=0; rejected();
    setup(); gcc[0x6c004/4]=UINT32_MAX; rejected();
    setup(); crm[0]=0; rejected();
    setup(); crm[0x208/4]=1; rejected(); /* Other channel is not empty. */
    puts("Ace5 CRM XNU: actual opt-in, quiet lease, cold domains, map and table admission passed");
}
static void retained(unsigned timeout)
{
    setup(); no_channel_ack=timeout; giulia_crm_test_init();
    assert(creates==1 && exits==1 && deallocs==1 && maps==7 && !unmaps && requests==1);
    assert(shared->request==1 && shared->ack==1 && shared->active==1 && gc.transport.owned);
    assert(gc.transport.published && crm[0x1020/4]==1);
    assert(complete==!timeout && gc.transport.pending==timeout);
    if (!timeout) assert(gc.transport.state==GIULIA_CRM_HELD && crm[0x1000/4]==1);
    assert(gc_write(&gc,0,0)==-1 && gc_write(&gc,0x1020,0)==-1);
    assert(gc_write(&gc,0x200,1)==-1 && gc_write(&gc,0x208,0)==-1);
    static const uint32_t po[]={0,0x40,0x80,0x600,0x640},bo[]={0xc0,0x140,0x680,0x700,0x780};
    for (unsigned ch=0;ch<2;ch++) for (unsigned st=0;st<5;st++) {
        for (unsigned i=0;i<2;i++) assert(!crm[(0x200+po[st]+4*(ch*2+i))/4]);
        for (unsigned i=0;i<16;i++) assert(!crm[(0x200+bo[st]+4*(ch*16+i))/4]);
    }
    giulia_crm_test_init(); assert(creates==1 && requests==1);
    puts(timeout ? "Ace5 CRM XNU: actual timeout keeps mappings, channel and firmware quiet" :
        "Ace5 CRM XNU: actual zero-table channel ACK and retained lifetime passed");
}
int main(int argc,char **argv)
{
    if (argc==1) admission();
    else if (argc==2 && !strcmp(argv[1],"--retained")) retained(0);
    else if (argc==2 && !strcmp(argv[1],"--timeout")) retained(1);
    else assert(!"unknown test scenario");
    return 0;
}
