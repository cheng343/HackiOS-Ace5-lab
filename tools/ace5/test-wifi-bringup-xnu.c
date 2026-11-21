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
#include "../../xnu/files/bsd/sys/giulia_gunyah_watchdog.h"
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
typedef struct { unsigned size; } SHA256_CTX;
static void SHA256_Init(SHA256_CTX *c) { c->size = 0; }
static void SHA256_Update(SHA256_CTX *c, const void *data, size_t size)
{ assert(data && size == 80); c->size += size; }
static void SHA256_Final(void *out, SHA256_CTX *c)
{ assert(c->size == 80); memset(out, 0, 32); }
static struct { unsigned num_cpus; } topology;
#define ml_get_topology_info() (&topology)
static unsigned enabled, board, cpus, cpu, seconds, ceiling, other, crm_all, sm_diag, watchdog;
static unsigned creates, exits, deallocs, maps, unmaps, fail_map, time_us;
static unsigned no_ack, no_resume_ack, pending_irq, lost_guard;
static unsigned stop_calls, qos_calls, gem_calls, crm_calls;
static unsigned allow_prepare, bcm_policy_seen, crm_patches_seen, cx_level_seen, rpmh_cx_sel_seen;
static unsigned cx_nominal_unavailable;
static int start_rc, stop_rc, qos_rc, gem_rc, crm_rc;
static uint64_t shared_pa=0x90000100;
static uint8_t pdc_table[508];
static uint32_t gic_enabled;
static uint64_t watchdog_ctl;
static unsigned watchdog_reads, watchdog_writes, watchdog_cannot_cancel;
static unsigned wdt_call_count, wdt_fail_at, wdt_disables, wdt_competing;
#define WDT_RAW_HIGH UINT64_C(0xa5a5beef00000000)
static unsigned cold_reset, mock_reset_response, loader_returns;
static unsigned root_id_reads, pcs_first_arg, pre_phy_clks_arg, drop_pre_phy_clock;
static unsigned keep_sls_arg, suppress_sls_latch;
static unsigned endpoint_identifies, endpoint_releases, endpoint_reasserts, x1_id_arg, native_width_arg;
enum pin_fault { PIN_OK, HELD_READ_FAIL, HELD_EN_LOW, HELD_PERST_HIGH,
    RELEASE_READ_FAIL, RELEASE_PERST_LOW, RELEASE_EN_LOW, REASSERT_STUCK_HIGH };
static enum pin_fault injected_pin_fault;
/* Set by the fake GPIO, not by the code under test. Both sinks must stay
 * silent between a successful release and the return of a permitted reassert. */
static int ep_critical, identify_rc, lose_owner, release_rc;
/* Independent live-state changes in the mocked controller, not flags set in the adapter. */
enum lease_event { LEASE_IDLE, LEASE_CONTROL, ACK_LOST, QUIET_LOST, IRQ_ACTIVE, IRQ_ENABLED, RESET_REQ_LOST };
static enum lease_event lease_event;
static unsigned first_lease_op;
static unsigned ep_loader_calls, ep_console_calls;
static char messages[16384];
static struct { uintptr_t va; vm_size_t size; } allocation[24];
static void wb_test_barrier(void);
static void delay(unsigned us) { time_us+=us; assert(time_us<=30000000); }
static uint64_t wb_test_watchdog_call(uint64_t id, uint64_t a1, uint64_t a2,
    uint64_t *r1, uint64_t *r2)
{
    if (++wdt_call_count == wdt_fail_at) {
        if (wdt_competing) watchdog_ctl = GIULIA_WDT_ENABLED;
        return WDT_RAW_HIGH | UINT32_MAX;
    }
    if (id == GIULIA_WDT_STATUS) {
        watchdog_reads++;
        assert(!a1 && !a2 && r1);
        *r1 = WDT_RAW_HIGH | watchdog_ctl;
        if (r2) *r2 = WDT_RAW_HIGH | 7;
    } else {
        watchdog_writes++;
        assert(!r1 && !r2);
        if (id == GIULIA_WDT_CONTROL) {
            assert((a1 == 3 || a1 == 2) && !a2);
            if (a1 == 2) {
                wdt_disables++;
                if (watchdog_cannot_cancel) return WDT_RAW_HIGH | 1;
            }
            watchdog_ctl = a1 == 3 ? GIULIA_WDT_ENABLED : 0;
        } else {
            assert(id == GIULIA_WDT_SET_TIME || (id == GIULIA_WDT_PET && watchdog_ctl));
        }
    }
    return WDT_RAW_HIGH;
}
static int hackios_platform(void) { return (int)board; }
static int PE_parse_boot_argn(const char *name,void *out,unsigned size)
{
    unsigned value=0;
    if (!strcmp(name,"ace5_cesta_profile")) return 0;
    assert(size==4);
    if(!strcmp(name,"ace5_wifi_bringup"))value=enabled;
    if(!strcmp(name,"cpus"))value=cpus;
    if(!strcmp(name,"bootcpu"))value=cpu;
    if(!strcmp(name,"loader_return_after"))value=seconds;
    if(!strcmp(name,"ace5_wifi_bringup_stage"))value=ceiling;
    if(!strcmp(name,"ace5_wifi_crm_all_states"))value=crm_all;
    if(!strcmp(name,"ace5_wifi_sm_diag"))value=sm_diag;
    if(!strcmp(name,"ace5_wifi_watchdog"))value=watchdog;
    if(!strcmp(name,"ace5_wifi_cold_reset"))value=cold_reset;
    if(!strcmp(name,"ace5_wifi_pcs_first"))value=pcs_first_arg;
    if(!strcmp(name,"ace5_wifi_x1_id"))value=x1_id_arg;
    if(!strcmp(name,"ace5_wifi_keep_sls"))value=keep_sls_arg;
    if(!strcmp(name,"ace5_wifi_native_width"))value=native_width_arg;
    if(!strcmp(name,"ace5_wifi_pre_phy_clks"))value=pre_phy_clks_arg;
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
static int quiet_printf(const char *format,...)
{ (void)format; assert(!ep_critical); ep_console_calls++; return 0; }
#define printf quiet_printf
#include "../../xnu/files/bsd/dev/giulia_wifi_bringup_xnu.c"
#undef printf

static void wb_test_barrier(void)
{
    if (drop_pre_phy_clock && wb.parf.address) {
        uint32_t *sys = (uint32_t *)wb.parf.address;
        if (*sys == 0x365e) { *sys &= ~(1u << 6); drop_pre_phy_clock = 0; }
    }
    if(wb.quiet && !no_ack && !(no_resume_ack && !wb.quiet->request)) {
        wb.quiet->ack=wb.quiet->request;wb.quiet->active=wb.quiet->request;
        if (mock_reset_response && wb.quiet->reset_request)
            wb.quiet->reset_ack = mock_reset_response;
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
{ assert(!ep_critical); ep_loader_calls++;
  assert(strlen(messages)+strlen(s)<sizeof(messages));strcat(messages,s); }
void loader_return(void) { loader_returns++; }
int giulia_pcie_power_pipe_ready(struct giulia_pcie_power *p, uint32_t *cbcr)
{ assert(p == &wb.bringup.supply.power); *cbcr = 0x10c00; return GIULIA_POWER_OK; }
int giulia_pcie_link_read_root_id(struct giulia_pcie_link *s, uint32_t flags)
{
    assert(flags == GIULIA_LINK_PREPARE_REQUIRED && s == &wb.bringup.link);
    assert(!wb.atu.address && !wb.cfg.address && !wb.bar0.address);
    root_id_reads++;
    s->root_id = wb_link_r32(0, GIULIA_LINK_DBI, 0);
    return GIULIA_LINK_OK;
}
int giulia_pcie_link_prepare(struct giulia_pcie_link *s, uint32_t flags)
{
    assert(wb.sm_diag == WB_SM_ENDPOINT && flags ==
        (GIULIA_LINK_PREPARE_REQUIRED | GIULIA_LINK_EXACT_ROOT_ID | GIULIA_LINK_VENDOR_TRAINING |
         (wb.native_width ? GIULIA_LINK_NATIVE_WIDTH : 0u) |
         (wb.x1_id ? GIULIA_LINK_NATIVE_X1_ID : 0u) |
         (wb.keep_sls ? GIULIA_LINK_PRESERVE_SPEED_CAP : 0u) |
         (wb.stock_train ? GIULIA_LINK_STOCK_TRAIN_LANES : 0u)));
    s->preserve_speed_cap = !!(flags & GIULIA_LINK_PRESERVE_SPEED_CAP) && !suppress_sls_latch;
    s->snapshot_lanes = 2;
    s->train_lanes_verified = 0;
    assert(wb.dbi.address && wb.atu.address && wb.cfg.address && !wb.bar0.address);
    s->root_id = wb_link_r32(0, GIULIA_LINK_DBI, 0);
    s->state = GIULIA_LINK_PREPARED;
    return 0;
}
int giulia_wifi_gpio_snapshot(const struct giulia_wifi_gpio *g, struct giulia_wifi_gpio_signals *pins)
{
    assert(wb_guard() && wb.quiet->reset_ack == LOADER_QUIET_RESET_ARMED);
    assert(g->state == GIULIA_WGPIO_ON || g->state == GIULIA_WGPIO_RELEASED);
    int held = g->state == GIULIA_WGPIO_ON;
    if ((held && injected_pin_fault == HELD_READ_FAIL) ||
        (!held && injected_pin_fault == RELEASE_READ_FAIL)) return GIULIA_WGPIO_READBACK;
    *pins = (struct giulia_wifi_gpio_signals){
        .wlan_en = (held && injected_pin_fault == HELD_EN_LOW) ||
            (!held && injected_pin_fault == RELEASE_EN_LOW) ? 2u : 3u,
        .perst = held && injected_pin_fault == HELD_PERST_HIGH ? 1u :
            *(uint32_t *)(wb.tlmm.address + 94u * 0x1000u + 4u)
    };
    return 0;
}
int giulia_wifi_gpio_perst_release(struct giulia_wifi_gpio *g, uint32_t flags)
{
    assert(flags == (GIULIA_WGPIO_RELEASE_REQUIRED | GIULIA_WGPIO_BT_CONTROL_EXCLUSIVE));
    assert(g->state == GIULIA_WGPIO_ON && wb.quiet->reset_ack == LOADER_QUIET_RESET_ARMED);
    endpoint_releases++;
    assert(!ep_critical);
    if (release_rc) return release_rc;
    ep_critical = 1;
    *(uint32_t *)(wb.tlmm.address + 94u * 0x1000u + 4u) =
        injected_pin_fault == RELEASE_PERST_LOW ? 2u : 3u;
    g->state = GIULIA_WGPIO_RELEASED;
    return 0;
}
int giulia_wifi_gpio_perst_assert(struct giulia_wifi_gpio *g, uint32_t flags)
{
    assert(flags == (GIULIA_WGPIO_BEGIN_REQUIRED | GIULIA_WGPIO_BT_CONTROL_EXCLUSIVE));
    assert((g->state == GIULIA_WGPIO_RELEASED || g->state == GIULIA_WGPIO_ON) &&
        wb.endpoint_reset_unconfirmed);
    endpoint_reasserts++;
    *(uint32_t *)(wb.tlmm.address + 94u * 0x1000u + 4u) =
        injected_pin_fault == REASSERT_STUCK_HIGH ? 1u : 0u;
    g->state = GIULIA_WGPIO_ON;
    ep_critical = 0;
    return 0;
}
/* Only fixture RAM is seeded/inspected directly. All access under test must
 * pass through the production callbacks. An all-ones read alone is not proof:
 * writes must also leave the sentinel untouched. */
static void exercise_ep_callback(unsigned op, unsigned allowed)
{
    const unsigned off = 0x380;
    const unsigned regions[] = { GIULIA_LINK_PARF, GIULIA_LINK_DBI,
        GIULIA_LINK_ATU, GIULIA_LINK_CFG };
    for (unsigned i=0; i < (op < 4 ? 4u : 1u); i++) {
        unsigned reg = regions[i];
        struct wb_map *m = op < 4 ? link_reg(reg) : &wb.tlmm;
        assert(m->address && m->size >= off+4);
        uint32_t *word = (uint32_t *)(m->address+off);
        const uint32_t saved = *word, initial = 0x12345678u;
        *word = initial;
        switch (op) {
        case 0: assert(wb_link_r32(0,reg,off)==(allowed ? initial : UINT32_MAX)); break;
        case 1: wb_link_w32(0,reg,off,0x31415926u);
                assert(*word==(allowed ? 0x31415926u : initial)); break;
        case 2: assert(wb_link_r16(0,reg,off)==(allowed ? 0x5678u : UINT16_MAX)); break;
        case 3: wb_link_w16(0,reg,off,0x7777);
                assert(*word==(allowed ? 0x12347777u : initial)); break;
        case 4: assert(wb_gpio_r(0,off)==(allowed ? initial : UINT32_MAX)); break;
        case 5: wb_gpio_w(0,off,0x87654321u);
                assert(*word==(allowed ? 0x87654321u : initial)); break;
        default: assert(0);
        }
        *word = saved;
    }
}
static void mock_lease_transition(void)
{
    if (lease_event == LEASE_IDLE) return;
    if (lease_event == LEASE_CONTROL) {
        for (unsigned op=0; op<6; op++) exercise_ep_callback(op,1);
        assert(!wb.guard_failed); return;
    }
    /* Stop the fixture from automatically rewriting lost ACKs on barriers. */
    no_ack=1;
    uint32_t *gic=(uint32_t *)wb.gic.address;
    switch (lease_event) {
    case ACK_LOST: wb.quiet->reset_ack=0; break;
    case QUIET_LOST: wb.quiet->active=0; break;
    case IRQ_ACTIVE: gic[0x304/4] |= WB_IRQ; break;
    case IRQ_ENABLED: gic_enabled |= WB_IRQ; gic[0x104/4]=gic_enabled; break;
    case RESET_REQ_LOST: wb.quiet->reset_request=0; break;
    default: assert(0);
    }
    if (first_lease_op < 6) {
        exercise_ep_callback(first_lease_op,0);  /* first observer must latch it */
        assert(wb.guard_failed);
    }
    /* No legal reassert now: the adapter may report only cached failed evidence. */
    ep_critical=0;
}

int giulia_pcie_link_identify(struct giulia_pcie_link *s, uint32_t flags)
{
    assert(flags == GIULIA_LINK_TRAIN_REQUIRED && wb.bringup.supply.gpio.state == GIULIA_WGPIO_RELEASED);
    assert(wb.endpoint_reset_unconfirmed);
    assert(ep_critical);
    endpoint_identifies++;
    s->train_parf_valid = GIULIA_LINK_TRAIN_PARF_ENABLE_VALID;
    s->failure_stage = 0;
    if (identify_rc) {
        s->stage = s->failure_stage = GIULIA_LINK_STAGE_TRAIN;
        s->endpoint_id = s->id_lanes = s->train_lanes_verified = 0;
        s->train_samples = s->polls = s->train_ltssm_seen[0] = 0;
        s->train_ltssm_first = s->train_ltssm_last = 0;
        s->link_status = 0;
        if (identify_rc == GIULIA_LINK_TIMEOUT) {
            s->train_parf_valid |= GIULIA_LINK_TRAIN_PARF_TIMEOUT_VALID;
            s->train_samples = s->polls = 3;
            s->train_ltssm_seen[0] = 1; /* three synthetic Detect.Quiet samples */
        } else {
            assert(identify_rc == GIULIA_LINK_READBACK);
            s->train_parf_valid = 0;
        }
        if (lose_owner) { wb.guard_failed = 1; ep_critical = 0; }
        return identify_rc;
    }
    s->endpoint_id = GIULIA_PCIE_LINK_EP_ID;
    s->link_status = 0x2011;
    s->id_lanes = wb.x1_id ? 1u : 0u;
    s->train_lanes_verified = !!wb.stock_train;
    s->train_lanes_before = 2;
    s->train_lanes_after = 1;
    s->train_samples = 2;
    s->train_ltssm_first = 0;
    s->train_ltssm_last = 17;
    s->train_ltssm_seen[0] = (1u << 17) | 1u;
    /* Adversarial successful core return after real mock loss; the adapter
     * cannot publish an endpoint or drive PERST simply because core said OK. */
    if (lease_event != LEASE_IDLE) {
        s->stage=GIULIA_LINK_STAGE_ENDPOINT;
        s->state=GIULIA_LINK_IDENTIFIED;
    }
    mock_lease_transition();
    return 0;
}

/* The adapter must propagate retained/failed core cleanup, independently of
 * whether its own IRQ and quiet handshakes would have succeeded. */
int giulia_wifi_bringup_start(struct giulia_wifi_bringup *b,const struct giulia_wifi_bringup_config *cfg)
{
    assert(wb_guard());b->cfg=*cfg;b->stage=cfg->max_stage?cfg->max_stage:GIULIA_BRINGUP_READY;
    if (sm_diag >= WB_SM_TABLES || watchdog) {
        assert(!wb.bar0.address);
        assert(!!wb.atu.address == (sm_diag == WB_SM_ENDPOINT));
        assert(!!wb.cfg.address == (sm_diag == WB_SM_ENDPOINT));
        assert(!!wb.dbi.address == (sm_diag >= WB_SM_DBI_ID));
    }
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
{
    assert(allow_prepare);
    if (bcm_policy_seen == GIULIA_BCM_PCIE0_FIXED_500_800_STOCK_STARTUP)
        assert(wb.early_reset_armed && wb.quiet->reset_ack == LOADER_QUIET_RESET_ARMED);
    crm_patches_seen=count; return 0;
}
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
    watchdog=watchdog_reads=watchdog_writes=watchdog_cannot_cancel=0;watchdog_ctl=0;
    wdt_call_count=wdt_fail_at=wdt_disables=wdt_competing=0;
    cold_reset=mock_reset_response=loader_returns=0;
    ep_critical=identify_rc=lose_owner=release_rc=0;
    lease_event=LEASE_IDLE; first_lease_op=0;
    ep_loader_calls=ep_console_calls=0;
    keep_sls_arg=suppress_sls_latch=0;
    root_id_reads=pcs_first_arg=pre_phy_clks_arg=drop_pre_phy_clock=x1_id_arg=native_width_arg=0;
    endpoint_identifies=endpoint_releases=endpoint_reasserts=0;injected_pin_fault=PIN_OK;
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
    setup();sm_diag=8;giulia_wifi_bringup_init();assert(!maps && !creates);
    setup();sm_diag=4;giulia_wifi_bringup_init();assert(!maps && !creates);
    setup();cold_reset=1;giulia_wifi_bringup_init();assert(!maps && !creates);
    /* The SLS choice does not require native-x1-ID or post-release lane training. */
    for (unsigned n=0;n<6;n++) {
        setup();sm_diag=7;seconds=60;cold_reset=native_width_arg=pcs_first_arg=keep_sls_arg=1;
        if (n==0) keep_sls_arg=2;
        if (n==1) pcs_first_arg=0;
        if (n==2) native_width_arg=0;
        if (n==3) cold_reset=0;
        if (n==4) ceiling=5;
        if (n==5) sm_diag=6;
        giulia_wifi_bringup_init();
        assert(!maps && !creates && strstr(messages,"keep SLS requires"));
    }
    for (unsigned x1=0;x1<=1;x1++) {
        setup();sm_diag=7;seconds=60;cold_reset=native_width_arg=pcs_first_arg=keep_sls_arg=1;
        x1_id_arg=x1;
        giulia_wifi_bringup_init();
        assert(!maps && !creates && strstr(messages,"CESTA board profile missing"));
    }
    setup();sm_diag=7;seconds=60;cold_reset=x1_id_arg=1;
    giulia_wifi_bringup_init();assert(!maps && !creates && strstr(messages,"native x1 ID requires"));
    setup();sm_diag=7;seconds=60;cold_reset=native_width_arg=1;x1_id_arg=2;
    giulia_wifi_bringup_init();assert(!maps && !creates && strstr(messages,"native x1 ID requires"));
    setup();sm_diag=7;seconds=60;cold_reset=native_width_arg=x1_id_arg=1;
    giulia_wifi_bringup_init();assert(!maps && !creates && strstr(messages,"CESTA board profile missing"));
    setup();pre_phy_clks_arg=1;giulia_wifi_bringup_init();assert(!maps && !creates);
    assert(strstr(messages, "pre-PHY clock hold requires"));
    setup();sm_diag=7;seconds=60;cold_reset=1;pre_phy_clks_arg=1;
    giulia_wifi_bringup_init();assert(!maps && !creates && strstr(messages, "pre-PHY clock hold requires"));
    setup();sm_diag=7;seconds=60;cold_reset=pcs_first_arg=1;pre_phy_clks_arg=2;
    giulia_wifi_bringup_init();assert(!maps && !creates && strstr(messages, "pre-PHY clock hold requires"));
    setup();sm_diag=7;seconds=60;cold_reset=pcs_first_arg=pre_phy_clks_arg=1;ceiling=5;
    giulia_wifi_bringup_init();assert(!maps && !creates && strstr(messages, "pre-PHY clock hold requires"));
    setup();sm_diag=7;seconds=60;cold_reset=pcs_first_arg=pre_phy_clks_arg=1;
    giulia_wifi_bringup_init();assert(!maps && !creates && strstr(messages, "CESTA board profile missing"));
    setup();sm_diag=3;giulia_wifi_bringup_init();assert(!maps && !creates);
    assert(strstr(messages, "CESTA board profile missing or mismatched"));
    setup();sm_diag=2;giulia_wifi_bringup_init();
    assert(stop_calls == 1 && maps == unmaps && strstr(messages, "cesta cold-check rc=-3"));
    assert(!strstr(messages, "requested stage complete"));
    setup();watchdog=2;giulia_wifi_bringup_init();assert(!maps && !creates && !watchdog_writes);
    setup();watchdog=1;ceiling=5;giulia_wifi_bringup_init();assert(!maps && !creates && !watchdog_writes);
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
    /* Ordinary callbacks still need no reset mailbox and no GIC mapping. */
    assert(!wb.quiet && !wb.gic.address);
    uint32_t gpio=0x12345678u; wb.tlmm.address=(uintptr_t)&gpio;
    wb_link_w32(0,GIULIA_LINK_DBI,0,0x87654321u);
    wb_link_w16(0,GIULIA_LINK_DBI,0,0xabcd);
    assert(wb_link_r16(0,GIULIA_LINK_DBI,0)==0xabcdu && id==0x8765abcdu);
    assert(wb_gpio_r(0,0)==gpio); wb_gpio_w(0,0,0x19283746u);
    assert(gpio==0x19283746u && !wb.guard_failed);
}
static void bar_callbacks(void)
{
    setup();
    wb_map(&wb.shared,shared_pa,sizeof(*wb.quiet),VM_WIMG_WCOMB);
    wb.quiet=(void *)wb.shared.address;
    wb_map(&wb.gic,0x17100000u,0x10000u,VM_WIMG_IO);
    wb_map(&wb.bar0,GIULIA_PCIE_LINK_BAR0_PA,GIULIA_PCIE_LINK_BAR0_SIZE,VM_WIMG_IO);
    wb.quiet->request=1; wb_test_barrier(); assert(wb_irq_acquire());
    struct wb_map bar=wb.bar0, shared=wb.shared, gic=wb.gic;
    uint32_t *cell=(void *)bar.address, *irq=(void *)gic.address;
    uint32_t val=0xdeadbeefu;
    static const uint32_t stages[]={GIULIA_BRINGUP_SOC_INFO,GIULIA_BRINGUP_MHI_SNAP,GIULIA_BRINGUP_READY};
    wb.bringup.link.state=GIULIA_LINK_READY;
    wb.bringup.link.core_owned=wb.bringup.link.memory_owned=wb.bringup.link.memory_enabled=1;
    for (unsigned i=0;i<sizeof(stages)/sizeof(stages[0]);i++) {
        wb.bringup.stage=stages[i];
        assert(!wb_bar_w32(0,0,0x11220000u+i) && !wb_bar_r32(0,0,&val) && val==0x11220000u+i);
    }
    assert(!wb_bar_w32(0,GIULIA_PCIE_LINK_BAR0_SIZE-4,9));
    assert(!wb_bar_r32(0,GIULIA_PCIE_LINK_BAR0_SIZE-4,&val) && val==9);
    /* Bad call and inactive/identity-only mode never acquire or poison an owner. */
    const uint32_t bad[]={1,GIULIA_PCIE_LINK_BAR0_SIZE,UINT32_MAX-3u};
    for (unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        assert(wb_bar_r32(0,bad[i],&val)<0 && val==9 && wb_bar_w32(0,bad[i],7)<0);
    }
    assert(wb_bar_r32(0,0,0)<0 && !wb.guard_failed && wb.bringup.stage==GIULIA_BRINGUP_READY);
    wb.sm_diag=WB_SM_ENDPOINT; wb.bringup.stage=GIULIA_BRINGUP_PHY_START; wb.bar0=(struct wb_map){0};
    assert(wb_bar_r32(0,0,&val)<0 && wb_bar_w32(0,0,99)<0 && !wb.guard_failed &&
        wb.bringup.stage==GIULIA_BRINGUP_PHY_START);
    wb.sm_diag=0;
    /* Alternate first access: neither read nor write may be the one that
     * reaches MMIO with an unsafe mapping, ownership or quiet/IRQ session. */
    for (unsigned fault=0;fault<16;fault++) for (unsigned wr=0;wr<2;wr++) {
        wb.bar0=bar; wb.shared=shared; wb.gic=gic; wb.quiet=(void *)shared.address;
        wb.guard_failed=0; wb.irq_dirty=1; wb.bringup.stage=stages[fault%3];
        wb.bringup.link.state=GIULIA_LINK_READY;
        wb.bringup.link.core_owned=wb.bringup.link.memory_owned=wb.bringup.link.memory_enabled=1;
        wb.quiet->ack=wb.quiet->active=wb.quiet->request=1;
        irq[0x304/4]=0; wb.sm_diag=0; no_ack=1;
        switch(fault) {
        case 0: wb.bar0=(struct wb_map){0}; break;
        case 1: wb.bar0.address+=4; break; /* last dword falls outside allocation */
        case 2: wb.bar0.allocation=0; break;
        case 3: wb.quiet=0; break;
        case 4: wb.shared.size=wb.shared.address-wb.shared.allocation+sizeof(*wb.quiet)-1; break;
        case 5: wb.gic.size=0x304; break;
        case 6: wb.gic.address=0; break;
        case 7: wb.quiet->active=0; break;
        case 8: wb.irq_dirty=0; break;
        case 9: irq[0x304/4]=WB_IRQ; break;
        case 10: wb.bringup.link.state=GIULIA_LINK_IDENTIFIED; break;
        case 11: wb.bringup.link.core_owned=0; break;
        case 12: wb.bringup.link.memory_owned=0; break;
        case 13: wb.bringup.link.memory_enabled=0; break;
        case 14: wb.sm_diag=WB_SM_ENDPOINT; break;
        case 15: wb.guard_failed=1; break;
        }
        *cell=0x11223344; val=0xdeadbeef;
        if(wr) assert(wb_bar_w32(0,0,77)<0); else assert(wb_bar_r32(0,0,&val)<0);
        assert(wb.guard_failed && wb.bringup.stage==GIULIA_BRINGUP_HELD);
        assert(wb_bar_w32(0,0,77)<0 && wb_bar_r32(0,0,&val)<0);
        assert(*cell==0x11223344 && val==0xdeadbeef);
    }
    puts("PASS: XNU BAR0 validates aperture, CPU decode and quiet/IRQ lease; lost sessions held without BAR access");
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

static void stock_startup_perf(void)
{
    setup();
    wb_map(&wb.shared, shared_pa, sizeof(struct loader_firmware_quiet), VM_WIMG_WCOMB);
    wb.quiet = (void *)wb.shared.address;
    wb_map(&wb.gic, 0x17100000u, 0x10000u, VM_WIMG_IO);
    wb_map(&wb.tlmm, GIULIA_WIFI_GPIO_PA, GIULIA_WIFI_GPIO_SIZE, VM_WIMG_IO);
    uint32_t *perst = (void *)(wb.tlmm.address + 94u * 0x1000u);
    wb.quiet->request = 1;
    wb_test_barrier();
    assert(wb_irq_acquire());
    wb.stock_perf = wb.cold_reset = allow_prepare = 1;
    wb.sm_diag = WB_SM_ENDPOINT;
    wb.bringup.stage = GIULIA_BRINGUP_NOC_STAGE;
    wb.bringup.supply.state = GIULIA_SUPPLY_HOST_PREPARED;
    wb.bringup.supply.power.state = GIULIA_POWER_HOST_PREPARED;
    wb.bringup.supply.gpio.state = GIULIA_WGPIO_ON;
    perst[0] = 1u << 9;
    perst[1] = 1;
    wb.quiet->reset_capability = 1;
    mock_reset_response = LOADER_QUIET_RESET_ARMED;
    assert(wb_prepare_noc(0, &wb.bringup.supply) != 0 && !crm_patches_seen && !wb.quiet->reset_request);
    perst[1] = 0;
    wb.quiet->reset_capability = 0;
    assert(wb_prepare_noc(0, &wb.bringup.supply) != 0 && !crm_patches_seen);
    wb.quiet->reset_capability = 1;
    assert(wb_prepare_noc(0, &wb.bringup.supply) == 0 && wb.early_reset_armed);
    assert(bcm_policy_seen == GIULIA_BCM_PCIE0_FIXED_500_800_STOCK_STARTUP);
    assert(crm_patches_seen == GIULIA_BCM_PCIE0_PATCHES);
    assert(wb_arm_early_reset() == 0);
    wb.quiet->reset_ack = LOADER_QUIET_RESET_FAILED;
    assert(wb_arm_early_reset() != 0);
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
    wb.sm_diag = WB_SM_TABLES;
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
    sm[0x1018 / 4] = 0;
    assert(wb_cesta_table_write(0, 0, 1) != 0 && sm[0] == 0);
    wb.sm_diag = WB_SM_STAGE;
    wb.cesta_profile[3] = 1;
    wb.cesta_profile[4 * (GIULIA_CESTA_SEQUENCE_WORDS + 1) - 1] = 4;
    assert(wb_cesta_table_write(0, GIULIA_CESTA_START_OFFSET, 1) != 0 &&
        sm[GIULIA_CESTA_START_OFFSET / 4] == 0);
    assert(wb_cesta_stage_and_discard() == GIULIA_CESTA_OK && !wb.guard_failed);
    assert(strstr(messages, "cesta stage rc=0 discard=0 cold=0") &&
        !strstr(messages, "cesta table write begin off=0x1090"));
    assert(sm[0] == 0 && sm[GIULIA_CESTA_BRANCH_OFFSET / 4] == 0);
    const unsigned test_begin = time_us;
    wb.watchdog_trial = WB_WATCHDOG_STATUS;
    assert(wb_watchdog_quiet_test() == 0 && watchdog_reads == 1 && !watchdog_writes);
    watchdog_ctl = GIULIA_WDT_ENABLED;
    assert(wb_watchdog_quiet_test() != 0 && watchdog_reads == 2 && !watchdog_writes);
    watchdog_ctl = 0;
    wb.watchdog_trial = WB_WATCHDOG_EXPIRY;
    assert(wb_watchdog_quiet_test() != 0 && watchdog_reads == 6 && watchdog_writes == 5);
    assert(time_us - test_begin == WB_WATCHDOG_WAIT_SECONDS * 1000000u && !watchdog_ctl);
    assert(!wb.guard_failed && strstr(messages,"quiet watchdog expiry not observed"));
    watchdog_ctl = GIULIA_WDT_ENABLED;
    /* Exercise the production SMC32 boundary directly with polluted upper
     * halves, including genuine low-word errors and both status payloads. */
    uint64_t wdt_ctl32 = 0, wdt_elapsed32 = 0;
    assert(!wb_watchdog_call(GIULIA_WDT_STATUS,0,0,&wdt_ctl32,&wdt_elapsed32));
    assert(wdt_ctl32==GIULIA_WDT_ENABLED && wdt_elapsed32==7);
    wdt_fail_at=wdt_call_count+1;
    assert(wb_watchdog_call(GIULIA_WDT_STATUS,0,0,&wdt_ctl32,&wdt_elapsed32)==UINT32_MAX);
    size_t saved_messages=strlen(messages);
    static const char *phases[]={"","","SET_BARK","SET_BITE","ENABLE","PET","STATUS"};
    for (unsigned n=2;n<=6;n++) {
        wdt_call_count=0; wdt_fail_at=n; wdt_competing=n<=4; watchdog_ctl=0;
        unsigned before=wdt_disables;
        assert(wb_watchdog_quiet_test()!=0 && !wb.guard_failed);
        char expected[64];
        snprintf(expected,sizeof(expected),"arm failed %s=0xffffffff",phases[n]);
        assert(strstr(messages+saved_messages,expected));
        messages[saved_messages]=0;
        if (wdt_competing) {
            assert(wdt_call_count==n && watchdog_ctl==GIULIA_WDT_ENABLED && wdt_disables==before);
        } else {
            /* PET or final STATUS failed after our successful ENABLE. */
            assert(wdt_call_count==n+3 && !watchdog_ctl && wdt_disables==before+1);
        }
    }
    wdt_fail_at=0; wdt_competing=0; watchdog_ctl=GIULIA_WDT_ENABLED;
    puts("PASS: XNU Giulia watchdog W0-W2 width and own-ENABLE-only cancellation");
    watchdog_cannot_cancel = 1;
    assert(wb_watchdog_cancel() != 0 && watchdog_ctl == GIULIA_WDT_ENABLED);
    wb.sm_diag = WB_SM_START;
    wb.cold_reset = 1;
    assert(wb_cesta_table_write(0, GIULIA_CESTA_START_OFFSET, 1) != 0);
    assert(wb_cesta_stage_and_discard() == GIULIA_CESTA_LEASE && !loader_returns && !wb.guard_failed);
    assert(sm[0] == 0 && sm[GIULIA_CESTA_START_OFFSET / 4] == 0);
    wb.quiet->reset_capability = 1;
    mock_reset_response = LOADER_QUIET_RESET_ARMED;
    assert(wb_cesta_stage_and_discard() == GIULIA_CESTA_RETAINED && loader_returns == 1 && wb.guard_failed);
    assert(sm[0] == 1 && sm[GIULIA_CESTA_START_OFFSET / 4] == 1);
    assert(strstr(messages, "cesta START result=1 observe=0; retained until reset"));
    wb.guard_failed = 0;
    wb_map(&wb.crm_c, WB_CRM_C_PHYS, WB_CRM_C_SIZE, VM_WIMG_IO);
    uint32_t *perf = (void *)wb.crm_c.address;
    uint32_t *parf = (void *)wb.parf.address;
    parf[0] = 0x361c;
    wb.sm_diag = WB_SM_PERF;
    assert(wb_cesta_pre_dbi() == 0 && !root_id_reads && parf[0] == 0x361c);
    wb_map(&wb.dbi, GIULIA_PCIE_LINK_DBI_PA, 0x1000u, VM_WIMG_IO);
    *(uint32_t *)wb.dbi.address = 0x011317cbu;
    wb.sm_diag = WB_SM_DBI_ID;
    assert(wb_cesta_pre_dbi() == GIULIA_LINK_PREREQUISITE && !root_id_reads);
    perf[0xc / 4] = GIULIA_PCIE_BOOT_PERF;
    assert(wb_cesta_pre_dbi() == 0 && root_id_reads == 1 && parf[0] == 0x365e);
    assert(wb.bringup.link.root_id == 0x011317cbu);
    /* A real stage-4 fixture snapshot, before the first endpoint transaction.
     * Never restore this while testing revocation within the same session. */
    assert(wb.bringup.link.state==GIULIA_LINK_PARF_READY && wb.bringup.link.parf_owned &&
        !wb.bringup.link.endpoint_id);
    const struct giulia_pcie_link initial_endpoint_link = wb.bringup.link;
    wb_map(&wb.atu, GIULIA_PCIE_LINK_ATU_PA, 0x1000u, VM_WIMG_IO);
    wb_map(&wb.cfg, GIULIA_PCIE_LINK_CFG_PA, GIULIA_PCIE_LINK_CFG_SIZE, VM_WIMG_IO);
    wb.sm_diag = WB_SM_ENDPOINT;
    wb.stock_perf = 1;
    perf[0x4 / 4] = GIULIA_PCIE_STOCK_STARTUP_PERF;
    assert(wb_cesta_pre_dbi() == GIULIA_LINK_PREREQUISITE && !endpoint_identifies);
    perf[0xc / 4] = GIULIA_PCIE_STOCK_STARTUP_PERF;
    perf[0x4 / 4] = GIULIA_PCIE_BOOT_PERF;
    assert(wb_cesta_pre_dbi() == GIULIA_LINK_PREREQUISITE && !endpoint_identifies);
    perf[0x4 / 4] = GIULIA_PCIE_STOCK_STARTUP_PERF;
    messages[0] = 0;
    unsigned log_before = ep_loader_calls, console_before = ep_console_calls;
    assert(wb_cesta_pre_dbi() == 0 && endpoint_identifies == 1);
    assert(ep_loader_calls > log_before && ep_console_calls > console_before && !ep_critical);
    const char *released_log = strstr(messages,"reset release rc=0 (deferred; ltssm_enable_readback=1)");
    const char *id_log = strstr(messages,"endpoint identify rc=0");
    const char *reassert_log = strstr(messages,"endpoint reset reassert rc=0");
    assert(released_log && id_log && reassert_log && released_log < id_log && id_log < reassert_log);
    assert(strstr(messages, "endpoint pins held en=0x3 sw=0 wlan_sw=0 perst=0 clkreq="));
    assert(strstr(messages, "PCIe training samples=2 first=0 last=17 seen_lo=0x20001 seen_hi=0"));
    assert(strstr(messages, "endpoint pins released en=0x3 sw=0 wlan_sw=0 perst=0x3 clkreq="));
    assert(strstr(messages, "endpoint pins after-training en=0x3 sw=0 wlan_sw=0 perst=0x3 clkreq="));
    assert(!wb.endpoint_reset_unconfirmed && !perst[1] && !wb.bar0.address);
    assert(wb.bringup.link.endpoint_id == GIULIA_PCIE_LINK_EP_ID);
    wb.stock_perf = 0;
    assert(!strstr(messages, "native endpoint width="));
    assert(!strstr(messages,"native speed cap="));
    wb.max_stage = GIULIA_BRINGUP_PHY_START;
    wb.native_width = wb.pcs_first = wb.keep_sls = 1;
    wb.stock_train = wb.x1_id = 1;
    perf[0xc / 4] = perf[0x4 / 4] = GIULIA_PCIE_BOOT_PERF;
    assert(wb_cesta_pre_dbi() == 0 && endpoint_identifies == 2);
    assert(strstr(messages, "stock train lanes before=2 after=1"));
    assert(strstr(messages, "native endpoint width=1"));
    assert(strstr(messages, "native train lanes snapshot=2"));
    assert(strstr(messages,"ace5-wifi-bringup: native speed cap=3 target=1\n"));
    /* Strict x2 mode is independent of the optional x1-ID and lane overrides. */
    wb.stock_train=wb.x1_id=0; messages[0]=0;
    assert(wb_cesta_pre_dbi()==0 && endpoint_identifies==3);
    assert(strstr(messages,"ace5-wifi-bringup: native speed cap=3 target=1\n") &&
        !strstr(messages,"native endpoint width="));
    suppress_sls_latch=1; messages[0]=0;
    assert(wb_cesta_pre_dbi()==GIULIA_LINK_PROFILE && endpoint_identifies==3);
    assert(!strstr(messages,"native speed cap="));
    suppress_sls_latch=0; wb.keep_sls=0; wb.stock_train=wb.x1_id=1;
    puts("PASS: XNU native speed cap is opt-in, guarded, and independent of native x1 ID");
    /* A low input with a high output latch is not a released endpoint.
     * Neither snapshot failures nor an unreliable WLAN_EN may start LTSSM. */
    static const struct { enum pin_fault fault; unsigned released; int error; } refusals[] = {
        {HELD_READ_FAIL, 0, GIULIA_LINK_READBACK}, {HELD_EN_LOW, 0, GIULIA_LINK_PREREQUISITE},
        {HELD_PERST_HIGH, 0, GIULIA_LINK_PREREQUISITE},
        {RELEASE_READ_FAIL, 1, GIULIA_LINK_READBACK}, {RELEASE_PERST_LOW, 1, GIULIA_LINK_PREREQUISITE},
        {RELEASE_EN_LOW, 1, GIULIA_LINK_PREREQUISITE}
    };
    for (unsigned i = 0; i < sizeof(refusals)/sizeof(refusals[0]); i++) {
        unsigned releases = endpoint_releases, reasserts = endpoint_reasserts;
        messages[0] = 0;
        injected_pin_fault = refusals[i].fault;
        assert(wb_cesta_pre_dbi() == refusals[i].error && endpoint_identifies == 3);
        assert(strstr(messages, refusals[i].released ? "pin gate refused released" : "pin gate refused held"));
        assert(!strstr(messages, "PCIe endpoint identify") && !strstr(messages, "PCIe training samples"));
        assert(!!strstr(messages,"reset release rc=0 (deferred; ltssm_enable_readback=0)") ==
            !!refusals[i].released);
        assert(!strstr(messages,"ltssm_enable_readback=1"));
        assert(!strstr(messages, "stock train lanes before=") && !strstr(messages, "native endpoint width="));
        assert(endpoint_releases == releases + refusals[i].released && endpoint_reasserts == reasserts + 1);
        assert(!wb.endpoint_reset_unconfirmed && !perst[1] && !wb.bar0.address);
    }
    injected_pin_fault = REASSERT_STUCK_HIGH;
    assert(wb_cesta_pre_dbi() == GIULIA_WGPIO_READBACK && endpoint_identifies == 4);
    assert(wb.endpoint_reset_unconfirmed && perst[1] == 1);
    assert(!strstr(messages, "native endpoint width="));
    wb.native_width = wb.stock_train = wb.x1_id = 0;
    /* End this synthetic stuck-pin scenario before testing unrelated leases. */
    perst[1] = 0; wb.endpoint_reset_unconfirmed = 0; injected_pin_fault = PIN_OK;
    /* Differentiate proof of LTSSM-enable readback from training/ID success. */
    wb.native_width = wb.stock_train = wb.x1_id = 1;
    for (unsigned n = 0; n < 2; n++) {
        unsigned releases = endpoint_releases, ids = endpoint_identifies, resets = endpoint_reasserts;
        messages[0] = 0;
        identify_rc = n ? GIULIA_LINK_TIMEOUT : GIULIA_LINK_READBACK;
        assert(wb_cesta_pre_dbi() == identify_rc);
        assert(!ep_critical && endpoint_releases == releases+1 && endpoint_identifies == ids+1 &&
            endpoint_reasserts == resets+1 && !wb.endpoint_reset_unconfirmed);
        released_log = strstr(messages, n ? "reset release rc=0 (deferred; ltssm_enable_readback=1)" :
                                            "reset release rc=0 (deferred; ltssm_enable_readback=0)");
        char expected[112];
        snprintf(expected,sizeof(expected),"endpoint identify rc=%d id=0 lnk=0 stage=%u fail=%u polls=%u",
            identify_rc,(unsigned)GIULIA_LINK_STAGE_TRAIN,(unsigned)GIULIA_LINK_STAGE_TRAIN,n ? 3u : 0u);
        id_log = strstr(messages,expected);
        reassert_log = strstr(messages,"endpoint reset reassert rc=0");
        assert(released_log && id_log && reassert_log && released_log < id_log && id_log < reassert_log);
        assert(strstr(messages, n ? "PCIe training samples=3 first=0 last=0 seen_lo=0x1" :
                                    "PCIe training samples=0 first=0 last=0 seen_lo=0"));
        assert(!strstr(messages,"endpoint identify rc=0") && !strstr(messages,"native endpoint width="));
    }
    unsigned ids = endpoint_identifies, resets = endpoint_reasserts;
    messages[0] = 0; identify_rc = 0; release_rc = GIULIA_WGPIO_READBACK;
    assert(wb_cesta_pre_dbi() == GIULIA_LINK_PREREQUISITE);
    char release_error[72];
    snprintf(release_error,sizeof(release_error),"endpoint reset release rc=%d\n",release_rc);
    assert(strstr(messages,release_error) && !strstr(messages,"(deferred;") &&
        !strstr(messages,"endpoint identify") && endpoint_identifies == ids &&
        endpoint_reasserts == resets+1 && !wb.endpoint_reset_unconfirmed);
    release_rc = 0; messages[0] = 0; resets = endpoint_reasserts;
    identify_rc = GIULIA_LINK_TIMEOUT; lose_owner = 1;
    assert(wb_cesta_pre_dbi() == GIULIA_LINK_PREREQUISITE);
    assert(endpoint_identifies == ids+1 && endpoint_reasserts == resets && wb.endpoint_reset_unconfirmed);
    assert(strstr(messages,"reset release rc=0 (deferred; ltssm_enable_readback=1)") &&
        strstr(messages,"endpoint identify rc=-7") && !strstr(messages,"endpoint reset reassert") &&
        !strstr(messages,"native endpoint width="));
    /* This was a synthetic lease-loss fixture, not a performed or inferred reset. */
    lose_owner = identify_rc = 0; wb.guard_failed = wb.endpoint_reset_unconfirmed = 0;
    wb.bringup.supply.gpio.state = GIULIA_WGPIO_ON; perst[1] = 0;
    wb.native_width = wb.stock_train = wb.x1_id = 0;
    puts("PASS: guarded release-to-reassert window is silent in both sinks; failures retain their evidence");
    /* Each iteration represents an independent synthetic boot/session. */
    wb.native_width=wb.x1_id=wb.stock_train=1;
    wb.bringup.link=initial_endpoint_link;
    lease_event=LEASE_CONTROL; messages[0]=0;
    assert(!wb_cesta_pre_dbi() && !wb.guard_failed && !wb.endpoint_reset_unconfirmed &&
        strstr(messages,"endpoint identify rc=0") && strstr(messages,"native endpoint width=1"));
    const struct { enum lease_event event; unsigned first; } losses[] = {
        {ACK_LOST,0}, {QUIET_LOST,1}, {IRQ_ACTIVE,2}, {IRQ_ENABLED,3},
        {RESET_REQ_LOST,4}, {ACK_LOST,5},
        {QUIET_LOST,6}, {ACK_LOST,6}, {IRQ_ACTIVE,6}
    };
    for (unsigned i=0; i<sizeof(losses)/sizeof(losses[0]); i++) {
        unsigned previous_ids=endpoint_identifies, previous_resets=endpoint_reasserts;
        /* The preceding independent case ended IDENTIFIED or FAULT; neither
         * may seed this case, or pre-DBI admission would skip fault injection. */
        wb.bringup.link=initial_endpoint_link;
        lease_event=losses[i].event; first_lease_op=losses[i].first; messages[0]=0;
        assert(wb_cesta_pre_dbi()==GIULIA_LINK_PREREQUISITE && wb.guard_failed);
        assert(endpoint_identifies==previous_ids+1 && endpoint_reasserts==previous_resets &&
            wb.endpoint_reset_unconfirmed && wb.bringup.link.endpoint_id==GIULIA_PCIE_LINK_EP_ID);
        assert(!wb.endpoint_io_window && wb.bringup.link.state==GIULIA_LINK_FAULT &&
            wb.bringup.link.failure_stage==GIULIA_LINK_STAGE_ENDPOINT &&
            wb.bringup.link.error==GIULIA_LINK_PREREQUISITE);
        assert(strstr(messages,"endpoint lease lost; retained until reset, prior ID is a sample only") &&
            !strstr(messages,"endpoint identify rc=0") && !strstr(messages,"native endpoint width=") &&
            !strstr(messages,"endpoint reset reassert"));
        /* Mimic firmware state looking valid again, NOT a new session token. */
        wb.quiet->active=wb.quiet->ack=wb.quiet->request=1;
        wb.quiet->reset_request=1; wb.quiet->reset_ack=LOADER_QUIET_RESET_ARMED;
        gic_enabled &= ~WB_IRQ; ((uint32_t *)wb.gic.address)[0x104/4]=gic_enabled;
        ((uint32_t *)wb.gic.address)[0x304/4] &= ~WB_IRQ;
        no_ack=0;
        for (unsigned op=0; op<6; op++) exercise_ep_callback(op,0);
        assert(wb_pcie_identify_endpoint()==GIULIA_LINK_PREREQUISITE);
        assert(wb.guard_failed && !wb.endpoint_io_window && wb.endpoint_reset_unconfirmed &&
            endpoint_identifies==previous_ids+1 && endpoint_reasserts==previous_resets);
        /* Explicit fixture reset, not device recovery or a successful reassert. */
        wb.guard_failed=wb.endpoint_reset_unconfirmed=0;
        wb.bringup.supply.gpio.state=GIULIA_WGPIO_ON; perst[1]=0;
    }
    wb.native_width=wb.x1_id=wb.stock_train=0; lease_event=LEASE_IDLE;
    puts("PASS: mode7 lease loss blocks the current and subsequent callbacks even after apparent recovery");
    wb.bringup.link=initial_endpoint_link; /* unrelated PHY/reset cases below */
    wb.bringup.pipe_checked = 0;
    wb.bringup.supply.power.state = GIULIA_POWER_READY;
    wb.pcs_first = 1;
    wb.quiet->reset_request = wb.quiet->reset_ack = 0;
    wb.quiet->reset_capability = 0;
    assert(wb_arm_pcs_first() != 0 && !wb.early_reset_armed && !wb.quiet->reset_request);
    wb.quiet->reset_capability = 1;
    assert(wb_arm_pcs_first() == 0 && wb.early_reset_armed && wb.quiet->reset_request == 1);
    assert(strstr(messages, "PCS-before-PIPE reset armed before PHY access"));
    /* No new write in the old profile, even with a confirmed reset lease. */
    parf[0] = 0x361c;
    assert(!wb.pre_phy_clks && !wb_arm_pcs_first() && parf[0] == 0x361c);
    wb.max_stage = GIULIA_BRINGUP_PHY_START;
    wb.pre_phy_clks = 1;
    messages[0] = 0;
    assert(wb_arm_pcs_first() != 0 && parf[0] == 0x361c); /* missing rollback slot */
    wb.bringup.link.parf_count = wb.bringup.link.saved_count = 1;
    wb.bringup.link.saved[0] = (struct giulia_pcie_link_saved){
        .region = GIULIA_LINK_PARF, .width = 4, .offset = 0, .value = 0x361c
    };
    wb.quiet->reset_ack = mock_reset_response = LOADER_QUIET_RESET_FAILED;
    assert(wb_arm_pcs_first() != 0 && parf[0] == 0x361c);
    wb.quiet->reset_ack = mock_reset_response = LOADER_QUIET_RESET_ARMED;
    perst[1] = 2;
    assert(wb_arm_pcs_first() != 0 && parf[0] == 0x361c);
    perst[1] = wb.guard_failed = 0;
    parf[0x1b0u / 4] = 1u << 8;
    assert(wb_arm_pcs_first() != 0 && parf[0] == 0x361c);
    parf[0x1b0u / 4] = wb.guard_failed = 0;
    parf[0] = UINT32_MAX;
    assert(wb_arm_pcs_first() != 0 && parf[0] == UINT32_MAX);
    parf[0] = 0x371c;
    assert(wb_arm_pcs_first() != 0 && parf[0] == 0x371c);
    parf[0] = 0x361c;
    drop_pre_phy_clock = 1;
    assert(wb_arm_pcs_first() != 0 && !drop_pre_phy_clock && parf[0] == 0x361e);
    assert(strstr(messages, "pre-PHY clock hold refused readback") &&
        !strstr(messages, "pre-PHY clock hold before="));
    parf[0] = 0x361c;
    assert(!wb_arm_pcs_first() && parf[0] == 0x365e &&
        wb.bringup.link.saved[0].value == 0x361c);
    assert(strstr(messages, "pre-PHY clock hold before=0x361c after=0x365e rc=0"));
    assert(!wb_arm_pcs_first() && parf[0] == 0x365e &&
        strstr(messages, "pre-PHY clock hold before=0x365e after=0x365e rc=0"));
    puts("Wi-Fi XNU pre-PHY clock hold: guarded ordering, readback and default-off passed");
    wb.pre_phy_clks = 0;
    assert(wb_arm_cold_reset(0) != 0);
    wb.bringup.pipe_checked = 1;
    assert(wb_arm_cold_reset(0) == 0);
    wb.early_reset_armed = wb.pcs_first = 0;
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
    admission();cleanup();single_access();bar_callbacks();noc_cleanup();noc_prepare();stock_startup_perf();noc_after_phy();setup();
    puts("Wi-Fi XNU adapter: one requested DBI read, stage markers, truthful cleanup, retained mappings/IRQ/quiet and ordered NoC stop passed");
    puts("Wi-Fi XNU adapter: CESTA cold check under PERST# with no DBI mapping or table writes passed");
    puts("Wi-Fi XNU adapter: quiet watchdog read-only status, bounded expiry and disarm passed");
    puts("Wi-Fi XNU adapter: CESTA current PERF gate, PARF clock bits and single DBI read passed");
    puts("Wi-Fi XNU adapter: guarded Gen1 endpoint read and PERST restoration passed");
    return 0;
}
