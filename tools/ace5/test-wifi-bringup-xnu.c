/* SPDX-License-Identifier: BSD-2-Clause */
/* Cloud-only production XNU adapter regression. RAM MMIO and loader peer;
 * lower-level bringup is stubbed here and tested by its separate fixture. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "../../xnu/files/bsd/sys/loader_firmware_quiet.h"
#define GIULIA_WIFI_BRINGUP_XNU_HOST_TEST
#define __unused __attribute__((unused))
#define __printflike(a,b)
#define KERN_SUCCESS 0
#define kSuccess 0
#define HACKIOS_PLATFORM_GIULIA 2
#define VM_WIMG_IO 7u
#define VM_WIMG_WCOMB 6u
#define trunc_page(x) ((uintptr_t)(x)&~(uintptr_t)0x3fff)
#define round_page(x) (((uintptr_t)(x)+0x3fff)&~(uintptr_t)0x3fff)
typedef uintptr_t vm_offset_t;
typedef uintptr_t vm_size_t;
typedef int kern_return_t;
typedef int wait_result_t;
typedef void *thread_t;
typedef int DTEntry;
static struct { unsigned num_cpus; } topology;
#define ml_get_topology_info() (&topology)
static unsigned enabled, board, cpus, cpu, seconds, ceiling, other, crm_all, sm_diag;
static unsigned creates, exits, deallocs, maps, unmaps, fail_map, time_us;
static unsigned no_ack, no_resume_ack, pending_irq, lost_guard;
static unsigned stop_calls, qos_calls, gem_calls, crm_calls;
static unsigned allow_prepare, bcm_policy_seen, crm_patches_seen, cx_level_seen, rpmh_cx_sel_seen;
static unsigned cx_nominal_unavailable;
static int start_rc, stop_rc, qos_rc, gem_rc, crm_rc;
static uint64_t shared_pa=0x90000100;
static uint8_t pdc_table[508];
static uint32_t gic_enabled;
static char messages[16384];
static struct { uintptr_t va; vm_size_t size; } allocation[24];
static void wb_test_barrier(void);
static void delay(unsigned us) { time_us+=us; assert(time_us<=3000000); }
static int hackios_platform(void) { return (int)board; }
static int PE_parse_boot_argn(const char *name,void *out,unsigned size)
{
    unsigned value=0; assert(size==4);
    if(!strcmp(name,"ace5_wifi_bringup"))value=enabled;
    if(!strcmp(name,"cpus"))value=cpus;
    if(!strcmp(name,"bootcpu"))value=cpu;
    if(!strcmp(name,"loader_return_after"))value=seconds;
    if(!strcmp(name,"ace5_wifi_bringup_stage"))value=ceiling;
    if(!strcmp(name,"ace5_wifi_crm_all_states"))value=crm_all;
    if(!strcmp(name,"ace5_wifi_sm_diag"))value=sm_diag;
    if(!strcmp(name,"ace5_aop_channel_test"))value=other;
    *(unsigned *)out=value; return value!=0;
}
static int SecureDTLookupEntry(void *root,const char *path,DTEntry *entry)
{ (void)root; assert(!strcmp(path,"/chosen")); *entry=1; return 0; }
static int SecureDTGetProperty(DTEntry entry,const char *name,const void **out,unsigned *size)
{
    assert(entry==1);
    if(!strcmp(name,"loader-firmware-quiet")){*out=&shared_pa;*size=sizeof(shared_pa);}
    else {assert(!strcmp(name,"loader-wlan-pdc-table"));*out=pdc_table;*size=sizeof(pdc_table);}
    return 0;
}
static thread_t current_thread(void) { return (void *)(uintptr_t)1; }
static void thread_deallocate(thread_t thread) { assert(thread==current_thread());deallocs++; }
static int kernel_thread_start(void (*fn)(void *,wait_result_t),void *arg,thread_t *out)
{ creates++;*out=current_thread();fn(arg,0);return 0; }
static int quiet_printf(const char *format,...) { (void)format;return 0; }
#define printf quiet_printf
#include "../../xnu/files/bsd/dev/giulia_wifi_bringup_xnu.c"
#undef printf

static void wb_test_barrier(void)
{
    if(wb.quiet && !no_ack && !(no_resume_ack && !wb.quiet->request)) {
        wb.quiet->ack=wb.quiet->request;wb.quiet->active=wb.quiet->request;
    }
    if(wb.gic.address) {
        uint32_t *g=(void *)wb.gic.address;
        if(g[0x184/4]) {
            assert(g[0x184/4]==WB_IRQ);gic_enabled&=~WB_IRQ;
            g[0x184/4]=0;g[0x104/4]=gic_enabled;
        }
        if(g[0x104/4]!=gic_enabled) { assert(g[0x104/4]==WB_IRQ);gic_enabled|=WB_IRQ; }
        g[0x104/4]=gic_enabled;
    }
}
vm_offset_t ml_io_map_unmappable(vm_offset_t pa,vm_size_t size,unsigned flags)
{
    assert(maps<24 && size && (flags==VM_WIMG_IO || flags==VM_WIMG_WCOMB));
    if(fail_map && maps+1==fail_map)return 0;
    void *base=calloc(1,size);assert(base);
    allocation[maps].va=(uintptr_t)base;allocation[maps].size=size;maps++;
    if(pa==0x90000000) {
        struct loader_firmware_quiet *q=(void *)((uint8_t *)base+0x100);
        q->magic=LOADER_QUIET_MAGIC;q->version=LOADER_QUIET_VERSION;q->size=sizeof(*q);
    }
    if(pa==0x17100000) {
        uint32_t *g=base;g[0xffe8/4]=0x3b;g[0x104/4]=gic_enabled;
    }
    return (uintptr_t)base;
}
void ml_io_unmap(vm_offset_t va,vm_size_t size)
{
    unsigned i=0;while(i<maps && allocation[i].va!=va)i++;
    assert(i<maps && allocation[i].size==size);unmaps++;
    /* Keep allocations until fixture reset so retained leases are inspectable. */
}
kern_return_t thread_terminate(thread_t thread) { assert(thread==current_thread());exits++;return 0; }
void loader_log_line(const char *s)
{ assert(strlen(messages)+strlen(s)<sizeof(messages));strcat(messages,s); }

/* The adapter must propagate retained/failed core cleanup, independently of
 * whether its own IRQ and quiet handshakes would have succeeded. */
int giulia_wifi_bringup_start(struct giulia_wifi_bringup *b,const struct giulia_wifi_bringup_config *cfg)
{
    assert(wb_guard());b->cfg=*cfg;b->stage=cfg->max_stage?cfg->max_stage:GIULIA_BRINGUP_READY;
    if (sm_diag == WB_SM_TABLES)
        assert(wb.pcie_sm.address && !wb.dbi.address && !wb.atu.address && !wb.cfg.address && !wb.bar0.address);
    if(start_rc){b->stage=GIULIA_BRINGUP_FAULT;b->cleanup_error=GIULIA_BRINGUP_SUPPLY;}
    return start_rc;
}
int giulia_wifi_bringup_stop(struct giulia_wifi_bringup *b)
{
    stop_calls++;assert(wb_guard());
    b->stage=stop_rc<0?GIULIA_BRINGUP_FAULT:stop_rc?GIULIA_BRINGUP_HELD:GIULIA_BRINGUP_STOPPED;
    b->cleanup_error=stop_rc<0?stop_rc:0;
    if(pending_irq)((uint32_t *)wb.gic.address)[0x204/4]=WB_IRQ;
    if(lost_guard)wb.guard_failed=1;
    return stop_rc;
}
const char *giulia_kiwi_fw_name(const struct giulia_kiwi_soc_info *s) { (void)s;return "amss20.bin"; }
int giulia_pcie_qos_stop(struct giulia_pcie_qos *p)
{ qos_calls++;if(!qos_rc)p->state=GIULIA_QOS_STOPPED;return qos_rc; }
int giulia_gem_clock_stop(struct giulia_gem_clock *p)
{ gem_calls++;assert(!qos_rc);if(!gem_rc)p->state=GIULIA_GEM_STOPPED;return gem_rc; }
int giulia_crm_stop(struct giulia_crm *p)
{ crm_calls++;assert(!qos_rc && !gem_rc);if(crm_rc>=0)p->state=crm_rc?GIULIA_CRM_HELD:GIULIA_CRM_STOPPED;return crm_rc; }
int giulia_bcm_pcie0_plan(__unused const uint8_t *db,__unused uint32_t bytes,
    __unused uint32_t version,unsigned policy,struct giulia_bcm_pcie0_plan *plan)
{
    assert(allow_prepare);
    bcm_policy_seen=policy;
    plan->patch_count=(policy==GIULIA_BCM_PCIE0_FIXED_500_800_ALL_STATES) ?
        GIULIA_BCM_PCIE0_ALL_STATES_PATCHES : GIULIA_BCM_PCIE0_PATCHES;
    return 0;
}
int giulia_crm_start_empty_keep(__unused struct giulia_crm *p,__unused const struct giulia_crm_io *io,
    __unused const struct giulia_crm_patch *patch,unsigned count)
{ assert(allow_prepare); crm_patches_seen=count; return 0; }
int giulia_arc_resolve(__unused const uint8_t *db,__unused uint32_t bytes,__unused uint32_t version,
    const char *name,struct giulia_arc_resource *r)
{
    assert(allow_prepare);
    r->address=!strcmp(name,"xo.lvl") ? 0x30004u : 0x30000u;
    r->count=6;
    r->level[2]=64u;
    r->level[5]=256u;
    return 0;
}
int giulia_arc_select(const struct giulia_arc_resource *r,uint32_t level,uint32_t *out)
{
    assert(allow_prepare);
    cx_level_seen=level;
    if (cx_nominal_unavailable && level>=256u) return GIULIA_RPMH_NOT_FOUND;
    *out=level>=256u ? 5u : 2u;
    assert(r->level[*out] >= level);
    return 0;
}
int giulia_rpmh_write(__unused struct giulia_rpmh *p,const struct giulia_rpmh_command *cmd,unsigned n)
{
    assert(allow_prepare && n==2 && cmd[0].data==3u);
    rpmh_cx_sel_seen=cmd[1].data;
    return 0;
}
int giulia_gem_clock_start(__unused struct giulia_gem_clock *p,__unused const struct giulia_gem_clock_io *io)
{ assert(allow_prepare); return 0; }
int giulia_pcie_qos_start(__unused struct giulia_pcie_qos *p,__unused const struct giulia_pcie_qos_io *io,__unused uint32_t conditions)
{ assert(allow_prepare); return 0; }

static void setup(void)
{
    for(unsigned i=0;i<maps;i++)free((void *)allocation[i].va);
    memset(allocation,0,sizeof(allocation));memset(&wb,0,sizeof(wb));wb_started=0;
    memset(wb_dbg_r32,0,sizeof(wb_dbg_r32));memset(wb_dbg_r16,0,sizeof(wb_dbg_r16));
    messages[0]=0;board=2;enabled=cpus=cpu=topology.num_cpus=1;seconds=30;ceiling=4;other=0;crm_all=sm_diag=0;
    creates=exits=deallocs=maps=unmaps=fail_map=time_us=0;
    no_ack=no_resume_ack=pending_irq=lost_guard=0;gic_enabled=WB_IRQ|1;
    stop_calls=qos_calls=gem_calls=crm_calls=0;start_rc=stop_rc=qos_rc=gem_rc=crm_rc=0;
    allow_prepare=bcm_policy_seen=crm_patches_seen=cx_level_seen=rpmh_cx_sel_seen=cx_nominal_unavailable=0;
}
static void admission(void)
{
    setup();enabled=0;giulia_wifi_bringup_init();assert(!maps && !creates);
    setup();board=1;giulia_wifi_bringup_init();assert(!maps && !creates);
    setup();cpus=2;giulia_wifi_bringup_init();assert(!maps && !creates);
    setup();cpu=0;giulia_wifi_bringup_init();assert(!maps && !creates);
    setup();seconds=0;giulia_wifi_bringup_init();assert(!maps && !creates);
    setup();ceiling=11;giulia_wifi_bringup_init();assert(!maps && !creates);
    setup();sm_diag=1;ceiling=5;giulia_wifi_bringup_init();assert(!maps && !creates);
    setup();sm_diag=1;crm_all=1;giulia_wifi_bringup_init();assert(!maps && !creates);
    setup();sm_diag=2;ceiling=5;giulia_wifi_bringup_init();assert(!maps && !creates);
    setup();sm_diag=3;giulia_wifi_bringup_init();assert(!maps && !creates);
    setup();sm_diag=2;giulia_wifi_bringup_init();
    assert(stop_calls == 1 && maps == unmaps && strstr(messages, "cesta cold-check rc=-3"));
    assert(!strstr(messages, "requested stage complete"));
    setup();other=1;giulia_wifi_bringup_init();assert(!maps && !creates);
    setup();no_ack=1;giulia_wifi_bringup_init();assert(maps==unmaps && !stop_calls);
    setup();fail_map=1;giulia_wifi_bringup_init();assert(!maps && exits==1);
}
static void cleanup(void)
{
    setup();giulia_wifi_bringup_init();
    assert(creates==1 && exits==1 && deallocs==1 && stop_calls==1 && maps==unmaps);
    assert(gic_enabled==(WB_IRQ|1));
    assert(strstr(messages,"requested stage complete; stage=4 max=4"));
    assert(strstr(messages,"cleanup complete; IRQ37 restored and firmware resumed"));
    setup();stop_rc=GIULIA_BRINGUP_RETAINED;giulia_wifi_bringup_init();
    assert(maps && !unmaps && wb.quiet->request==1 && wb_quiet_ok() && wb.irq_dirty);
    assert(!(gic_enabled&WB_IRQ) && strstr(messages,"resources retained;") && !strstr(messages,"cleanup complete;"));
    setup();stop_rc=GIULIA_BRINGUP_SUPPLY;giulia_wifi_bringup_init();
    assert(!unmaps && wb_quiet_ok() && wb.irq_dirty && strstr(messages,"cleanup FAILED;") && !strstr(messages,"cleanup complete;"));
    setup();start_rc=GIULIA_BRINGUP_LINK;giulia_wifi_bringup_init();
    assert(!stop_calls && !unmaps && wb_quiet_ok() && !strstr(messages,"requested stage complete;"));
    setup();pending_irq=1;giulia_wifi_bringup_init();
    assert(!unmaps && wb_quiet_ok() && wb.irq_dirty && strstr(messages,"cleanup FAILED;"));
    setup();lost_guard=1;giulia_wifi_bringup_init();assert(!unmaps && wb_quiet_ok() && wb.irq_dirty);
    setup();no_resume_ack=1;giulia_wifi_bringup_init();
    assert(!unmaps && strstr(messages,"restoration unconfirmed") && !strstr(messages,"cleanup complete;"));
}
static void single_access(void)
{
    /* Only DBI is mapped: an eager PARF/ATU/CFG/SM snapshot would fault. */
    setup();uint32_t id=0x011317cb;wb.dbi.address=(uintptr_t)&id;
    assert(wb_link_r32(0,GIULIA_LINK_DBI,0)==id);
    assert(strstr(messages,"DBI+0 access begin") && strstr(messages,"DBI+0 access returned"));
    id=UINT32_MAX;assert(wb_link_r32(0,GIULIA_LINK_DBI,0)==UINT32_MAX);
    assert(!wb.cfg.address && !wb.atu.address && !wb.parf.address);
}
static void noc_cleanup(void)
{
    for(unsigned failure=0;failure<4;failure++) {
        setup();wb.qos.state=GIULIA_QOS_ACTIVE;wb.gem_clk.state=GIULIA_GEM_ACTIVE;wb.crm_dev.state=GIULIA_CRM_ACTIVE;
        if(failure==1)qos_rc=GIULIA_QOS_READBACK;
        if(failure==2)gem_rc=GIULIA_GEM_READBACK;
        if(failure==3)crm_rc=GIULIA_CRM_TIMEOUT;
        int rc=wb_release_noc(0);assert((rc<0)==(failure!=0));
        assert(qos_calls==1 && gem_calls==(failure!=1) && crm_calls==(failure==0 || failure==3));
    }
    setup();wb.crm_dev.state=GIULIA_CRM_ACTIVE;crm_rc=GIULIA_CRM_RETAINED;
    assert(wb_release_noc(0)==GIULIA_BRINGUP_RETAINED && crm_calls==1);
}
static void noc_prepare(void)
{
    struct giulia_wifi_supply supply={0};
    setup();allow_prepare=1;wb.crm_all_states=0;
    assert(wb_prepare_noc(0,&supply)==0);
    assert(bcm_policy_seen==GIULIA_BCM_PCIE0_FIXED_500_800 && crm_patches_seen==GIULIA_BCM_PCIE0_PATCHES);
    assert(cx_level_seen==256u && rpmh_cx_sel_seen==5u); /* Never down-volt HLOS Nominal CX. */

    setup();allow_prepare=1;wb.crm_all_states=1;
    assert(wb_prepare_noc(0,&supply)==0);
    assert(bcm_policy_seen==GIULIA_BCM_PCIE0_FIXED_500_800_ALL_STATES &&
           crm_patches_seen==GIULIA_BCM_PCIE0_ALL_STATES_PATCHES);
    assert(cx_level_seen==256u && rpmh_cx_sel_seen==5u);

    setup();allow_prepare=1;cx_nominal_unavailable=1;
    assert(wb_prepare_noc(0,&supply)==GIULIA_RPMH_NOT_FOUND);
    assert(cx_level_seen==256u && !crm_patches_seen && !rpmh_cx_sel_seen);
}

static void noc_after_phy(void)
{
    setup();
    wb_map(&wb.shared, shared_pa, sizeof(struct loader_firmware_quiet), VM_WIMG_WCOMB);
    wb.quiet = (void *)wb.shared.address;
    wb_map(&wb.gic, 0x17100000u, 0x10000u, VM_WIMG_IO);
    wb_map(&wb.crm, GIULIA_NOC_CRM_PHYS, GIULIA_NOC_CRM_SIZE, VM_WIMG_IO);
    wb_map(&wb.gcc, GIULIA_QOS_GCC_PHYS, GIULIA_QOS_GCC_SIZE, VM_WIMG_IO);
    uint32_t *crm = (void *)wb.crm.address;
    uint32_t *gcc = (void *)wb.gcc.address;
    wb.quiet->request = 1;
    wb_test_barrier();
    assert(wb_irq_acquire());
    wb.crm_dev.state = GIULIA_CRM_ACTIVE;
    wb.crm_dev.target = 0;
    crm[0x1000 / 4] = 1;
    crm[0x1020 / 4] = 1;
    crm[0x3f0 / 4] = 4;
    gcc[0x52000 / 4] = (1u << 12) | (1u << 19) | (1u << 22);
    gcc[0x52008 / 4] = 0x7f;
    gcc[0x10058 / 4] = 0x80000002u;
    assert(wb_verify_noc(0) == GIULIA_BRINGUP_OK);
    assert(strstr(messages, "pwr_idx_raw=0x4"));
    assert(!strstr(messages, "post-phy cesta"));
    wb_map(&wb.pcie_sm, GIULIA_NOC_SM_PHYS, GIULIA_NOC_SM_SIZE, VM_WIMG_IO);
    uint32_t *sm = (void *)wb.pcie_sm.address;
    wb.sm_diag = 1;
    sm[0x1090 / 4] = 1;
    sm[0x1000 / 4] = 4;
    assert(wb_verify_noc(0) == GIULIA_BRINGUP_OK);
    assert(strstr(messages, "post-phy cesta start=0x1 branch0=0x4 override=0 clkreq=0"));
    sm[0x1090 / 4] = UINT32_MAX;
    assert(wb_verify_noc(0) == GIULIA_CRM_IO);
    wb_map(&wb.tlmm, GIULIA_WIFI_GPIO_PA, GIULIA_WIFI_GPIO_SIZE, VM_WIMG_IO);
    wb_map(&wb.parf, 0x01c00000u, 0x3000u, VM_WIMG_IO);
    uint32_t *perst = (void *)(wb.tlmm.address + 94u * 0x1000u);
    wb.bringup.stage = GIULIA_BRINGUP_PHY_START;
    wb.bringup.pipe_checked = 1;
    wb.bringup.link.state = GIULIA_LINK_PARF_READY;
    wb.bringup.link.parf_owned = 1;
    wb.bringup.supply.gpio.state = GIULIA_WGPIO_ON;
    perst[0] = (1u << 9);
    sm[0x1090 / 4] = 0;
    sm[0x1000 / 4] = 0;
    assert(wb_cesta_read_cold() == GIULIA_CESTA_OK);
    assert(strstr(messages, "cesta read begin off=0x30"));
    assert(strstr(messages, "cesta read begin off=0x1018"));
    sm[0x1018 / 4] = 4;
    assert(wb_cesta_read_cold() == GIULIA_CESTA_BUSY && sm[0x1018 / 4] == 4);
    sm[0x1018 / 4] = UINT32_MAX;
    assert(wb_cesta_read_cold() == GIULIA_CESTA_IO && sm[0x1018 / 4] == UINT32_MAX);
    perst[1] = 2;
    assert(wb_cesta_read_cold() == GIULIA_CESTA_LEASE && wb.guard_failed);
    wb.guard_failed = 0;
    wb.sm_diag = 0;
    crm[0x1000 / 4] = 2;
    assert(wb_verify_noc(0) == GIULIA_CRM_DRIFT);
    crm[0x1000 / 4] = 1;
    gcc[0x52008 / 4] &= ~(1u << 6);
    assert(wb_verify_noc(0) == GIULIA_POWER_READBACK);
    gcc[0x52008 / 4] = 0x7f;
    gcc[0x10058 / 4] = UINT32_MAX;
    assert(wb_verify_noc(0) == GIULIA_POWER_READBACK);
    no_ack = 1;
    wb.quiet->active = 0;
    assert(wb_verify_noc(0) == GIULIA_CRM_LEASE && wb.guard_failed);
}
int main(void)
{
    admission();cleanup();single_access();noc_cleanup();noc_prepare();noc_after_phy();setup();
    puts("Wi-Fi XNU adapter: one requested DBI read, stage markers, truthful cleanup, retained mappings/IRQ/quiet and ordered NoC stop passed");
    puts("Wi-Fi XNU adapter: CESTA cold check under PERST# with no DBI mapping or table writes passed");
    return 0;
}
