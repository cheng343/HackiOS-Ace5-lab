/* SPDX-License-Identifier: BSD-2-Clause */
/* Finite, single-CPU XNU bring-up diagnostic for Ace 5 Wi-Fi:
 * supply (KEEP_VOTES) -> PCIe power reset -> PARF -> QMP PHY/PIPE -> DBI -> PERST# ->
 * Link train -> BAR0 MEM decode -> WCN7850 SoC ID (BAR+0x224) -> MHI snapshot.
 * Explicitly gated behind ace5_wifi_bringup=1, cpus=1, bootcpu=1, finite timer,
 * loader firmware-quiet and IRQ37 isolation; cold reboot required afterwards.
 */
#ifndef GIULIA_WIFI_BRINGUP_XNU_HOST_TEST
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/crypto/sha2.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <sys/giulia_gunyah_watchdog.h>
#include <sys/hackios_platform.h>
#include <sys/loader_firmware_quiet.h>
#include <sys/systm.h>
#include <vm/memory_types.h>
#endif

#include "giulia_wifi_bringup.h"
#include "giulia_arc.h"
#include "giulia_bcm.h"
#include "giulia_gem_clock.h"
#include "giulia_pcie_cesta.h"
#include "giulia_pcie_noc.h"
#include "giulia_pcie_qos.h"

extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, unsigned int);
extern void ml_io_unmap(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);
extern void loader_log_line(const char *);
extern void loader_return(void);

#define WB_IRQ (1u << 5)
/* Observed Ace 5 HLOS DRV2 CX handoff is selector 5 (corner 256 = Nominal).
 * Never overwrite DRV2 with LOWER (64) and undervolt VDD_CX across the SoC. */
#define WB_CX_NOMINAL_LEVEL 256u
#define WB_SM_TABLES 2u
#define WB_SM_STAGE 3u
#define WB_SM_START 4u
#define WB_SM_PERF 5u
#define WB_SM_DBI_ID 6u
#define WB_SM_ENDPOINT 7u
#define WB_CRM_C_PHYS 0x01d05000u
#define WB_CRM_C_SIZE 0x2000u
#define WB_SM_PROFILE_BYTES (4u * GIULIA_CESTA_WORDS)
#define WB_WATCHDOG_STATUS 1u
#define WB_WATCHDOG_EXPIRY 2u
#define WB_WATCHDOG_WAIT_SECONDS 28u

struct wb_map { uintptr_t allocation, address; vm_size_t size; };
static struct {
    struct wb_map shared, gcc, tcsr, tlmm, rsc, db, gic, msg, ipcc;
    struct wb_map parf, phy, dbi, atu, cfg, bar0;
    struct wb_map crm, crm_c, qos_pcie, qos_gem, pcie_sm;
    volatile struct loader_firmware_quiet *quiet;
    struct giulia_wifi_bringup bringup;
    struct giulia_crm crm_dev;
    struct giulia_bcm_pcie0_plan crm_plan;
    struct giulia_gem_clock gem_clk;
    struct giulia_pcie_qos qos;
    uint8_t pdc_table[508];
    uint8_t cesta_profile[WB_SM_PROFILE_BYTES];
    uint32_t irq_dirty, irq_prior, guard_failed, endpoint_reset_unconfirmed, db_version, max_stage;
    uint32_t crm_all_states, sm_diag, watchdog_trial, cold_reset, cx_sel_voted, cx_level_voted;
    uint32_t pcs_first, early_reset_armed, native_width, stock_perf, stock_train;
    uint32_t noc_step, pre_phy_clks, x1_id, keep_sls, endpoint_io_window;
    int noc_rc, crm_rc, arc_rc, gem_rc, qos_rc;
} wb;

static unsigned wb_started;

static void wb_log(const char *fmt, ...) __printflike(1, 2);
static void wb_log(const char *fmt, ...)
{
    char line[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    loader_log_line(line);
    printf("%s", line);
}

static void wb_barrier(__unused void *cookie)
{
#ifdef GIULIA_WIFI_BRINGUP_XNU_HOST_TEST
    wb_test_barrier();
#else
    __asm__ volatile("dsb sy" ::: "memory");
#endif
}

static void wb_delay(__unused void *cookie, uint32_t us) { delay(us); }
static void wb_delay_u(__unused void *cookie, unsigned us) { delay(us); }

static void wb_map(struct wb_map *m, uint64_t pa, vm_size_t bytes, unsigned flags)
{
    vm_offset_t page = trunc_page(pa), off = pa - page;
    m->size = round_page(bytes + off);
    m->allocation = ml_io_map_unmappable(page, m->size, flags);
    if (m->allocation) m->address = m->allocation + off;
}

static void wb_unmap(struct wb_map *m)
{
    if (m->allocation) ml_io_unmap(m->allocation, m->size);
    *m = (struct wb_map){0};
}

static uint32_t wb_mmio(struct wb_map *m, uint32_t off)
{
    uint32_t v = *(volatile uint32_t *)(m->address + off);
    wb_barrier(0);
    return v;
}

static void wb_store(struct wb_map *m, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(m->address + off) = v;
    wb_barrier(0);
}

static int wb_quiet_ok(void)
{
    return wb.quiet->request == 1 && wb.quiet->ack == 1 && wb.quiet->active == 1;
}

static int wb_quiet_req(unsigned on)
{
    wb.quiet->request = on;
    wb_barrier(0);
    for (unsigned i = 0; i < 10000; i++) {
        if (wb.quiet->ack == on && wb.quiet->active == on) return 1;
        delay(100);
    }
    return 0;
}

static int wb_rwp(void)
{
    for (unsigned i = 0; i < 1000; i++) {
        uint32_t v = wb_mmio(&wb.gic, 0);
        if (v == UINT32_MAX) return 0;
        if (!(v & (1u << 31))) return 1;
        delay(10);
    }
    return 0;
}

static int wb_irq_acquire(void)
{
    uint32_t arch = wb_mmio(&wb.gic, 0xffe8), enabled = wb_mmio(&wb.gic, 0x104);
    uint32_t active = wb_mmio(&wb.gic, 0x304), pending = wb_mmio(&wb.gic, 0x204);
    if (!wb_quiet_ok() || arch == UINT32_MAX || enabled == UINT32_MAX ||
        active == UINT32_MAX || pending == UINT32_MAX ||
        ((arch & 0xf0) != 0x30 && (arch & 0xf0) != 0x40) ||
        ((active | pending) & WB_IRQ) || !wb_rwp()) return 0;
    wb.irq_prior = enabled & WB_IRQ;
    wb.irq_dirty = 1;
    wb_store(&wb.gic, 0x184, WB_IRQ);
    if (!wb_rwp()) return 0;
    enabled = wb_mmio(&wb.gic, 0x104);
    active = wb_mmio(&wb.gic, 0x304);
    return enabled != UINT32_MAX && active != UINT32_MAX && !((enabled | active) & WB_IRQ);
}

/* Unlike cfg_ok(), which validates negotiated PCIe link conditions,
 * this guard checks board ownership. Shared memory can invalidate a lease
 * without any PCIe access. Check it FIRST, before touching even the GIC. */
static int wb_guard_shared(void)
{
    return !wb.guard_failed && wb.irq_dirty && wb.quiet && wb_quiet_ok() &&
        (!wb.endpoint_io_window || (wb.quiet->reset_request == 1 &&
         wb.quiet->reset_ack == LOADER_QUIET_RESET_ARMED));
}

static int wb_guard(void)
{
    if (wb_guard_shared()) {
        uint32_t enabled = wb_mmio(&wb.gic, 0x104);
        if (enabled != UINT32_MAX && !(enabled & WB_IRQ) &&
            (!wb.endpoint_io_window || wb_guard_shared())) {
            uint32_t active = wb_mmio(&wb.gic, 0x304);
            if (active != UINT32_MAX && !(active & WB_IRQ) &&
                (!wb.endpoint_io_window || wb_guard_shared())) return 1;
        }
    }
    if (wb.endpoint_io_window) {
        wb.guard_failed = 1;
        wb.endpoint_reset_unconfirmed = 1; /* no further cleanup MMIO */
    }
    return 0;
}

/* Outside this mode-7 transaction the normal link/GPIO path incurs no
 * added MMIO. Once revoked, do not let later core rollback callbacks or a
 * briefly recovered shared flag turn the failed trial back into an owner.
 * A lease check cannot cancel an already issued MMIO transaction. */
static int wb_endpoint_io_ok(void)
{
    if (wb.endpoint_io_window) return wb_guard();
    return !(wb.sm_diag == WB_SM_ENDPOINT && wb.guard_failed);
}

static int wb_irq_release(void)
{
    uint32_t enabled, active, pending;
    if (!wb.irq_dirty) return 1;
    if (!wb_quiet_ok() || !wb_rwp()) return 0;
    enabled = wb_mmio(&wb.gic, 0x104);
    active = wb_mmio(&wb.gic, 0x304);
    pending = wb_mmio(&wb.gic, 0x204);
    if (enabled == UINT32_MAX || active == UINT32_MAX || pending == UINT32_MAX ||
        ((active | pending) & WB_IRQ)) return 0;
    if ((enabled & WB_IRQ) != wb.irq_prior) {
        wb_store(&wb.gic, wb.irq_prior ? 0x104 : 0x184, WB_IRQ);
        if (!wb_rwp()) return 0;
        enabled = wb_mmio(&wb.gic, 0x104);
        if (enabled == UINT32_MAX || (enabled & WB_IRQ) != wb.irq_prior) return 0;
    }
    wb.irq_dirty = 0;
    return 1;
}

static int wb_ext_supply(__unused void *cookie, __unused uint32_t req, __unused unsigned stage)
{ return wb_quiet_ok() ? 0 : -1; }

static uint32_t wb_gpio_r(void *c, uint32_t off) { (void)c; return wb_endpoint_io_ok() ? wb_mmio(&wb.tlmm, off) : UINT32_MAX; }
static void wb_gpio_w(void *c, uint32_t off, uint32_t v) { (void)c; if (wb_endpoint_io_ok()) wb_store(&wb.tlmm, off, v); }
static uint32_t wb_aop_r(void *c, uint32_t off) { (void)c; return wb_mmio(&wb.msg, off); }
static void wb_aop_w(void *c, uint32_t off, uint32_t v) { (void)c; wb_store(&wb.msg, off, v); }
static int wb_aop_kick(void *c) { (void)c; wb_store(&wb.ipcc, 0xcu, 0); return 0; }
static uint32_t wb_rpmh_r(void *c, uint32_t off)
{ (void)c; if (!wb_guard()) { wb.guard_failed = 1; return UINT32_MAX; } return wb_mmio(&wb.rsc, off); }
static void wb_rpmh_w(void *c, uint32_t off, uint32_t v)
{ (void)c; if (!wb_guard()) { wb.guard_failed = 1; return; } wb_store(&wb.rsc, off, v); }

static struct wb_map *pwr_blk(unsigned blk)
{ return blk == GIULIA_POWER_GCC ? &wb.gcc : &wb.tcsr; }
static uint32_t wb_pwr_r(void *c, unsigned blk, uint32_t off)
{ (void)c; return wb_mmio(pwr_blk(blk), off); }
static void wb_pwr_w(void *c, unsigned blk, uint32_t off, uint32_t v)
{ (void)c; wb_store(pwr_blk(blk), off, v); }

static struct wb_map *link_reg(unsigned reg)
{
    if (reg == GIULIA_LINK_PARF) return &wb.parf;
    if (reg == GIULIA_LINK_DBI) return &wb.dbi;
    if (reg == GIULIA_LINK_ATU) return &wb.atu;
    return &wb.cfg;
}
static uint32_t wb_dbg_r32[3], wb_dbg_r16[3];
static uint32_t wb_link_r32(void *c, unsigned r, uint32_t off)
{
    uint32_t v;
    (void)c;
    /* Only the core's requested access is allowed here. No diagnostic reads
     * of iATU/CFG/SM before admission, even when DBI returns all ones. A CPU
     * load can stall the bus, so record the checkpoint BEFORE issuing it. */
    if (!wb_endpoint_io_ok()) return UINT32_MAX;
    if (r == GIULIA_LINK_DBI && off == 0) {
        wb_log("ace5-wifi-bringup: DBI+0 access begin\n");
        /* Logging can block: revalidate before this one dangerous load. */
        if (!wb_endpoint_io_ok()) return UINT32_MAX;
    }
    v = wb_mmio(link_reg(r), off);
    wb_dbg_r32[0] = r; wb_dbg_r32[1] = off; wb_dbg_r32[2] = v;
    if (r == GIULIA_LINK_DBI && off == 0)
        wb_log("ace5-wifi-bringup: DBI+0 access returned %#x\n", v);
    return v;
}
static void wb_link_w32(void *c, unsigned r, uint32_t off, uint32_t v) { (void)c; if (wb_endpoint_io_ok()) wb_store(link_reg(r), off, v); }
static uint16_t wb_link_r16(void *c, unsigned r, uint32_t off)
{
    (void)c;
    if (!wb_endpoint_io_ok()) return UINT16_MAX;
    uint16_t v = *(volatile uint16_t *)(link_reg(r)->address + off);
    wb_barrier(0);
    wb_dbg_r16[0] = r; wb_dbg_r16[1] = off; wb_dbg_r16[2] = v;
    return v;
}
static void wb_link_w16(void *c, unsigned r, uint32_t off, uint16_t v)
{
    (void)c;
    if (!wb_endpoint_io_ok()) return;
    *(volatile uint16_t *)(link_reg(r)->address + off) = v;
    wb_barrier(0);
}

static uint32_t wb_phy_r(void *c, uint32_t off) { (void)c; return wb_mmio(&wb.phy, off); }
static void wb_phy_w(void *c, uint32_t off, uint32_t v) { (void)c; wb_store(&wb.phy, off, v); }
/* wb_map.size covers the whole page-aligned allocation, not the usable tail
 * starting at address. Never let an absent or short mapping reach wb_mmio. */
static int wb_map_covers(const struct wb_map *m, vm_size_t bytes)
{
    if (!bytes || !m->allocation || m->address < m->allocation || (m->address & 3u) ||
        bytes - 1 > UINTPTR_MAX - m->address) return 0;
    uintptr_t bias = m->address - m->allocation;
    return bias <= m->size && bytes <= m->size - bias;
}

static int wb_bar_admit(uint32_t off)
{
    if ((off & 3u) || off > GIULIA_PCIE_LINK_BAR0_SIZE - sizeof(uint32_t)) return 0;
    uint32_t stage = wb.bringup.stage;
    if (stage != GIULIA_BRINGUP_SOC_INFO && stage != GIULIA_BRINGUP_MHI_SNAP &&
        stage != GIULIA_BRINGUP_READY) return 0;

    const struct giulia_pcie_link *link = &wb.bringup.link;
    if (!wb.guard_failed && wb.irq_dirty && wb.sm_diag < WB_SM_TABLES &&
        !wb.watchdog_trial && !wb.cold_reset &&
        link->state == GIULIA_LINK_READY && link->core_owned && link->memory_owned &&
        link->memory_enabled && wb_map_covers(&wb.bar0, GIULIA_PCIE_LINK_BAR0_SIZE) &&
        wb_map_covers(&wb.shared, sizeof(*wb.quiet)) &&
        (uintptr_t)wb.quiet == wb.shared.address && wb_map_covers(&wb.gic, 0x308u) &&
        wb_quiet_ok() && wb_guard())
        return 1;

    /* This is a running session, not an invalid API request. Refuse the
     * operation and the ensuing stop() hardware cleanup under a lost lease.
     * No DBI, CFG or BAR probing can safely establish a missing lease. */
    wb.guard_failed = 1;
    giulia_wifi_bringup_hold_bar(&wb.bringup);
    return 0;
}

static int wb_bar_r32(__unused void *c, uint32_t off, uint32_t *val)
{
    if (!val || !wb_bar_admit(off)) return -1;
    *val = wb_mmio(&wb.bar0, off);
    return 0;
}

static int wb_bar_w32(__unused void *c, uint32_t off, uint32_t val)
{
    if (!wb_bar_admit(off)) return -1;
    wb_store(&wb.bar0, off, val);
    return 0;
}

static int wb_lease_ok(__unused void *c) { return wb_guard() ? 0 : -1; }
static int wb_crm_r(__unused void *c, uint32_t off, uint32_t *val)
{
    if (off >= GIULIA_NOC_CRM_SIZE || (off & 3u) || wb.guard_failed) return -1;
    *val = wb_mmio(&wb.crm, off);
    return *val == UINT32_MAX ? -1 : 0;
}
static int wb_crm_w(__unused void *c, uint32_t off, uint32_t val)
{
    if (off >= GIULIA_NOC_CRM_SIZE || (off & 3u) || !wb_guard()) return -1;
    wb_store(&wb.crm, off, val);
    return 0;
}
static int wb_gem_r(__unused void *c, uint32_t off, uint32_t *val)
{
    if (off >= GIULIA_GEM_GCC_SIZE || (off & 3u) || !wb_guard()) return -1;
    *val = wb_mmio(&wb.gcc, off);
    return *val == UINT32_MAX ? -1 : 0;
}
static int wb_gem_w(__unused void *c, uint32_t off, uint32_t val)
{
    if (off >= GIULIA_GEM_GCC_SIZE || (off & 3u) || !wb_guard()) return -1;
    wb_store(&wb.gcc, off, val);
    return 0;
}
static int wb_qos_r(__unused void *c, unsigned blk, uint32_t off, uint32_t *val)
{
    struct wb_map *m = blk == GIULIA_QOS_GCC ? &wb.gcc :
        blk == GIULIA_QOS_PCIE ? &wb.qos_pcie : blk == GIULIA_QOS_GEM ? &wb.qos_gem : 0;
    uint32_t lim = blk == GIULIA_QOS_GCC ? GIULIA_QOS_GCC_SIZE :
        blk == GIULIA_QOS_PCIE ? GIULIA_QOS_PCIE_SIZE : GIULIA_QOS_GEM_SIZE;
    if (!m || off >= lim || (off & 3u) || !wb_guard()) return -1;
    *val = wb_mmio(m, off);
    return 0;
}
static int wb_qos_w(__unused void *c, unsigned blk, uint32_t off, uint32_t val)
{
    struct wb_map *m = blk == GIULIA_QOS_GCC ? &wb.gcc :
        blk == GIULIA_QOS_PCIE ? &wb.qos_pcie : blk == GIULIA_QOS_GEM ? &wb.qos_gem : 0;
    uint32_t lim = blk == GIULIA_QOS_GCC ? GIULIA_QOS_GCC_SIZE :
        blk == GIULIA_QOS_PCIE ? GIULIA_QOS_PCIE_SIZE : GIULIA_QOS_GEM_SIZE;
    if (!m || off >= lim || (off & 3u) || !wb_guard()) return -1;
    wb_store(m, off, val);
    return 0;
}

static int wb_release_noc(__unused void *cookie)
{
    int rc;
    if (wb.qos.state != GIULIA_QOS_IDLE && wb.qos.state != GIULIA_QOS_STOPPED) {
        rc = giulia_pcie_qos_stop(&wb.qos);
        if (rc) return rc; /* Keep GEM/CRM and host clocks for a retry. */
    }
    if (wb.gem_clk.state != GIULIA_GEM_IDLE && wb.gem_clk.state != GIULIA_GEM_STOPPED) {
        rc = giulia_gem_clock_stop(&wb.gem_clk);
        if (rc) return rc;
    }
    if (wb.crm_dev.state != GIULIA_CRM_IDLE && wb.crm_dev.state != GIULIA_CRM_STOPPED) {
        rc = giulia_crm_stop(&wb.crm_dev);
        if (rc == GIULIA_CRM_RETAINED) return GIULIA_BRINGUP_RETAINED;
        if (rc) return rc;
    }
    return GIULIA_BRINGUP_OK;
}

static int wb_arm_early_reset(void);

/* Check the GPIO input and output latch without touching the controller.
 * The NoC stage uses this before PARF is available. */
static int wb_perst_gpio_low(void)
{
    if (!wb.tlmm.address) return 0;
    uint32_t cfg = wb_gpio_r(0, 94u * 0x1000u);
    uint32_t io = wb_gpio_r(0, 94u * 0x1000u + 4u);
    return (cfg & ((1u << 12) | (1u << 9) | (15u << 2))) == (1u << 9) && !(io & 3u);
}

static int wb_prepare_noc(__unused void *cookie, struct giulia_wifi_supply *supply)
{
    int rc;
    struct giulia_crm_io crm_io = {0, wb_crm_r, wb_crm_w, wb_barrier, wb_delay_u, wb_lease_ok};
    struct giulia_gem_clock_io gem_io = {
        0, wb_gem_r, wb_gem_w, wb_barrier, wb_delay_u, wb_lease_ok,
        GIULIA_GEM_GCC_PHYS, GIULIA_GEM_GCC_SIZE
    };
    struct giulia_pcie_qos_io qos_io = {
        0, wb_qos_r, wb_qos_w, wb_barrier, wb_lease_ok,
        {GIULIA_QOS_GCC_PHYS, GIULIA_QOS_PCIE_PHYS, GIULIA_QOS_GEM_PHYS},
        {GIULIA_QOS_GCC_SIZE, GIULIA_QOS_PCIE_SIZE, GIULIA_QOS_GEM_SIZE}
    };
    struct giulia_arc_resource xo_res = {0}, cx_res = {0};
    uint32_t cx_sel = 0;

    wb.noc_step = 1;
    unsigned crm_policy = wb.crm_all_states ?
        GIULIA_BCM_PCIE0_FIXED_500_800_ALL_STATES : GIULIA_BCM_PCIE0_FIXED_500_800;
    if (wb.stock_perf) crm_policy = GIULIA_BCM_PCIE0_FIXED_500_800_STOCK_STARTUP;
    rc = giulia_bcm_pcie0_plan((const uint8_t *)wb.db.address, GIULIA_RPMH_DB_MAX,
        wb.db_version, crm_policy, &wb.crm_plan);
    wb.crm_rc = rc;
    if (rc) { wb.noc_rc = rc; return rc; }

    wb.noc_step = 3;
    if ((rc = giulia_arc_resolve((const uint8_t *)wb.db.address, GIULIA_RPMH_DB_MAX,
            wb.db_version, "xo.lvl", &xo_res)) ||
        (rc = giulia_arc_resolve((const uint8_t *)wb.db.address, GIULIA_RPMH_DB_MAX,
            wb.db_version, "cx.lvl", &cx_res))) {
        wb.arc_rc = rc; wb.noc_rc = rc; return rc;
    }
    /* Keep DRV2 CX at or above Nominal (256, matching observed HLOS handoff
     * selector 5) instead of lowering VDD_CX to LOWER (64 / selector 2). */
    rc = giulia_arc_select(&cx_res, WB_CX_NOMINAL_LEVEL, &cx_sel);
    if (rc) {
        wb.arc_rc = rc; wb.noc_rc = rc; return rc;
    }

    wb.noc_step = 2;
    uint32_t patch_count = wb.crm_plan.patch_count ? wb.crm_plan.patch_count : GIULIA_BCM_PCIE0_PATCHES;
    if (wb.stock_perf) {
        if (supply != &wb.bringup.supply || wb.sm_diag != WB_SM_ENDPOINT || wb.crm_all_states ||
            wb.bringup.stage != GIULIA_BRINGUP_NOC_STAGE || supply->state != GIULIA_SUPPLY_HOST_PREPARED ||
            supply->power.state != GIULIA_POWER_HOST_PREPARED || supply->power.reset_attempted ||
            supply->gpio.state != GIULIA_WGPIO_ON || !wb_guard() || !wb_perst_gpio_low() || wb_arm_early_reset()) {
            wb.noc_rc = GIULIA_BRINGUP_PREREQUISITE;
            return wb.noc_rc;
        }
        wb_log("ace5-wifi-bringup: stock D0 reset armed before PERF vote\n");
    }
    rc = giulia_crm_start_empty_keep(&wb.crm_dev, &crm_io, wb.crm_plan.patch, patch_count);
    wb.crm_rc = rc;
    if (rc) { wb.noc_rc = rc; return rc; }

    wb.noc_step = 3;
    wb.cx_sel_voted = cx_sel;
    wb.cx_level_voted = cx_res.level[cx_sel];
    struct giulia_rpmh_command arc_cmds[2] = {
        {xo_res.address, 3u},
        {cx_res.address, cx_sel}
    };
    rc = giulia_rpmh_write(&supply->rpmh, arc_cmds, 2u);
    wb.arc_rc = rc;
    if (rc) { wb.noc_rc = rc; return rc; }

    wb.noc_step = 4;
    rc = giulia_gem_clock_start(&wb.gem_clk, &gem_io);
    wb.gem_rc = rc;
    if (rc) { wb.noc_rc = rc; return rc; }

    wb.noc_step = 5;
    rc = giulia_pcie_qos_start(&wb.qos, &qos_io, GIULIA_QOS_REQUIRED);
    wb.qos_rc = rc;
    if (rc) { wb.noc_rc = rc; return rc; }

    if (wb.crm.address && wb.gcc.address) {
        uint32_t pwr_idx = wb_mmio(&wb.crm, 0x3f0u);
        uint32_t cnoc_sf = wb_mmio(&wb.gcc, 0x10058u);
        wb_log("ace5-wifi-bringup: noc ready policy=%u patches=%u cx_sel=%u(lvl=%u) crm_pwr_idx=%#x cnoc_sf=%#x\n",
            crm_policy, patch_count, wb.cx_sel_voted, wb.cx_level_voted, pwr_idx, cnoc_sf);
    }
    wb.noc_step = 0; wb.noc_rc = 0;
    return 0;
}

static int wb_verify_noc(__unused void *cookie)
{
    const uint32_t required0 = (1u << 12) | (1u << 19) | (1u << 22);
    const uint32_t required1 = 0x7fu;
    uint32_t before_busy, before_update, after_busy, after_update, pwr_idx;
    uint32_t votes0, votes1, cnoc, aggregate, ddr;

    if (!wb_guard()) {
        wb.guard_failed = 1;
        return GIULIA_CRM_LEASE;
    }
    if (wb.crm_dev.state != GIULIA_CRM_ACTIVE || wb.crm_dev.target > 1u)
        return GIULIA_CRM_STATE;
    if (wb_crm_r(0, 0x1000u, &before_busy) || wb_crm_r(0, 0x1020u, &before_update) ||
        wb_crm_r(0, 0x3f0u, &pwr_idx))
        return GIULIA_CRM_IO;
    votes0 = wb_mmio(&wb.gcc, 0x52000u);
    votes1 = wb_mmio(&wb.gcc, 0x52008u);
    cnoc = wb_mmio(&wb.gcc, 0x10058u);
    aggregate = wb_mmio(&wb.gcc, 0x10064u);
    ddr = wb_mmio(&wb.gcc, 0x10074u);
    if (wb_crm_r(0, 0x1000u, &after_busy) || wb_crm_r(0, 0x1020u, &after_update))
        return GIULIA_CRM_IO;
    if (!wb_guard()) {
        wb.guard_failed = 1;
        return GIULIA_CRM_LEASE;
    }
    wb_log("ace5-wifi-bringup: post-phy noc target=%u busy=%#x/%#x update=%#x/%#x pwr_idx_raw=%#x\n",
        wb.crm_dev.target, before_busy, after_busy, before_update, after_update, pwr_idx);
    wb_log("ace5-wifi-bringup: post-phy gcc votes=%#x/%#x cnoc=%#x aggre=%#x ddr=%#x\n",
        votes0, votes1, cnoc, aggregate, ddr);

    if (votes0 == UINT32_MAX || votes1 == UINT32_MAX ||
        cnoc == UINT32_MAX || aggregate == UINT32_MAX || ddr == UINT32_MAX)
        return GIULIA_POWER_READBACK;
    uint32_t selected = 1u << wb.crm_dev.target;
    if (before_busy != selected || after_busy != selected ||
        before_update != selected || after_update != selected)
        return GIULIA_CRM_DRIFT;
    if ((votes0 & required0) != required0 || (votes1 & required1) != required1)
        return GIULIA_POWER_READBACK;
    if (wb.sm_diag) {
        uint32_t start = wb_mmio(&wb.pcie_sm, 0x1090u);
        if (start == UINT32_MAX) return GIULIA_CRM_IO;
        uint32_t branch = wb_mmio(&wb.pcie_sm, 0x1000u);
        if (branch == UINT32_MAX) return GIULIA_CRM_IO;
        uint32_t override = wb_mmio(&wb.pcie_sm, 0x1040u);
        if (override == UINT32_MAX) return GIULIA_CRM_IO;
        uint32_t clkreq = wb_mmio(&wb.pcie_sm, 0x109cu);
        if (clkreq == UINT32_MAX) return GIULIA_CRM_IO;
        if (!wb_guard()) {
            wb.guard_failed = 1;
            return GIULIA_CRM_LEASE;
        }
        wb_log("ace5-wifi-bringup: post-phy cesta start=%#x branch0=%#x override=%#x clkreq=%#x\n",
            start, branch, override, clkreq);
    }
    return GIULIA_BRINGUP_OK;
}

static int wb_perst_held(void)
{
    if (!wb_perst_gpio_low() || !wb_endpoint_io_ok() ||
        (wb_mmio(&wb.parf, 0x1b0u) & (1u << 8))) {
        wb.guard_failed = 1;
        return -1;
    }
    return 0;
}

static int wb_stage4_reset_lease(void)
{
    if (!wb_guard()) {
        wb.guard_failed = 1;
        return -1;
    }
    if (wb.bringup.stage != GIULIA_BRINGUP_PHY_START ||
        !wb.bringup.pipe_checked ||
        (wb.bringup.link.state != GIULIA_LINK_PARF_READY &&
         !(wb.sm_diag == WB_SM_ENDPOINT && wb.bringup.link.state == GIULIA_LINK_PREPARED)) ||
        !wb.bringup.link.parf_owned ||
        wb.bringup.supply.gpio.state != GIULIA_WGPIO_ON)
        return -1;
    return wb_perst_held();
}

static int wb_cesta_table_lease(__unused void *cookie)
{
    return !wb.pcie_sm.address ? -1 : wb_stage4_reset_lease();
}

static int wb_cesta_table_read(__unused void *cookie, uint32_t off, uint32_t *value)
{
    if ((off & 3u) || off >= GIULIA_NOC_SM_SIZE) return -1;
    if (wb.sm_diag == WB_SM_TABLES)
        wb_log("ace5-wifi-bringup: cesta read begin off=%#x\n", off);
    *value = wb_mmio(&wb.pcie_sm, off);
    if (*value)
        wb_log("ace5-wifi-bringup: cesta read off=%#x value=%#x\n", off, *value);
    return 0;
}

static int wb_cesta_table_write(void *cookie, uint32_t off, uint32_t value)
{
    const uint32_t branch = GIULIA_CESTA_BRANCH_OFFSET;
    if (wb.sm_diag >= WB_SM_START && off == GIULIA_CESTA_START_OFFSET) {
        if (value != 1 || wb.quiet->reset_ack != LOADER_QUIET_RESET_ARMED ||
            wb_cesta_table_lease(cookie))
            return -1;
        wb_log("ace5-wifi-bringup: cesta START write begin\n");
        wb_store(&wb.pcie_sm, off, value);
        wb_log("ace5-wifi-bringup: cesta START write returned\n");
        return 0;
    }
    if (wb.sm_diag < WB_SM_STAGE || (off & 3u) ||
        (off >= 4u * GIULIA_CESTA_SEQUENCE_WORDS &&
        (off < branch || off >= branch + 4u * GIULIA_CESTA_BRANCH_WORDS)) ||
        wb_cesta_table_lease(cookie))
        return -1;
    wb_log("ace5-wifi-bringup: cesta table write begin off=%#x\n", off);
    wb_store(&wb.pcie_sm, off, value);
    return 0;
}

static int wb_request_cold_reset(void)
{
    if (!wb.cold_reset || !wb.quiet->reset_capability ||
        wb.quiet->reset_request || wb.quiet->reset_ack)
        return -1;
    wb.quiet->reset_request = 1;
    wb_barrier(0);
    for (unsigned i = 0; i < 10000; i++) {
        uint32_t ack = wb.quiet->reset_ack;
        if (ack == LOADER_QUIET_RESET_ARMED) {
            wb_log("ace5-wifi-bringup: loader cold reset armed\n");
            return 0;
        }
        if (ack == LOADER_QUIET_RESET_FAILED) break;
        delay(100);
    }
    wb_log("ace5-wifi-bringup: loader cold reset arm unconfirmed\n");
    return -1;
}

static int wb_arm_early_reset(void)
{
    if (wb.early_reset_armed)
        return wb.quiet->reset_request == 1 &&
            wb.quiet->reset_ack == LOADER_QUIET_RESET_ARMED ? 0 : -1;
    if (wb_request_cold_reset()) return -1;
    wb.early_reset_armed = 1;
    return 0;
}

static int wb_arm_cold_reset(__unused void *cookie)
{
    if (wb_stage4_reset_lease()) return -1;
    if (wb.early_reset_armed) return wb_arm_early_reset();
    return wb_request_cold_reset();
}

/* The core snapshots PARF_SYS before prepare_parf changes bit 29. Its normal
 * link-stop path restores that snapshot. Do not touch a window merely because
 * the caller set parf_owned: require the actual rollback entry. */
static int wb_has_parf_sys_snapshot(void)
{
    const struct giulia_pcie_link *l = &wb.bringup.link;
    if (!l->parf_owned || l->parf_count > l->saved_count ||
        l->saved_count > sizeof(l->saved) / sizeof(l->saved[0])) return 0;
    for (unsigned i = 0; i < l->parf_count; i++) {
        const struct giulia_pcie_link_saved *v = &l->saved[i];
        if (v->region == GIULIA_LINK_PARF && v->width == 4 && v->offset == 0)
            return v->value != UINT32_MAX;
    }
    return 0;
}

static int wb_pre_phy_clocks_lease(void)
{
    return wb.pre_phy_clks == 1 && wb.max_stage == GIULIA_BRINGUP_PHY_START &&
        wb.sm_diag == WB_SM_ENDPOINT && wb.cold_reset == 1 && wb.pcs_first == 1 &&
        wb.early_reset_armed && wb.quiet && wb.quiet->reset_request == 1 &&
        wb.quiet->reset_ack == LOADER_QUIET_RESET_ARMED && wb_guard() &&
        wb.bringup.stage == GIULIA_BRINGUP_PHY_START &&
        wb.bringup.phy.state == GIULIA_PHY_IDLE && !wb.bringup.pipe_checked &&
        wb.bringup.supply.power.state == GIULIA_POWER_READY &&
        wb.bringup.supply.gpio.state == GIULIA_WGPIO_ON &&
        wb.bringup.link.state == GIULIA_LINK_PARF_READY && wb.parf.address &&
        wb_has_parf_sys_snapshot() && !wb_perst_held();
}

static int wb_pre_phy_clock_hold(void)
{
    /* The only experimental change is when we set L1_CLK_RMV_DIS and
     * CORE_CLK_CGC_DIS. The post-PHY DBI and PERF gates are independent. */
    const uint32_t mask = (1u << 1) | (1u << 6);
    if (!wb_pre_phy_clocks_lease()) goto refused;
    uint32_t before = wb_mmio(&wb.parf, 0);
    if (before == UINT32_MAX || (before & ~mask) != (0x365eu & ~mask)) {
        wb_log("ace5-wifi-bringup: pre-PHY clock hold refused unexpected SYS=0x%x\n", before);
        return -1;
    }
    if (!wb_pre_phy_clocks_lease()) goto refused;
    if ((before & mask) != mask)
        wb_store(&wb.parf, 0, before | mask);
    if (!wb_pre_phy_clocks_lease()) goto refused;
    uint32_t after = wb_mmio(&wb.parf, 0);
    if (after != (before | mask)) {
        wb_log("ace5-wifi-bringup: pre-PHY clock hold refused readback before=0x%x after=0x%x\n", before, after);
        return -1;
    }
    if (!wb_pre_phy_clocks_lease()) goto refused;
    wb_log("ace5-wifi-bringup: pre-PHY clock hold before=0x%x after=0x%x rc=0\n", before, after);
    return 0;
refused:
    wb_log("ace5-wifi-bringup: pre-PHY clock hold refused lease or snapshot\n");
    return -1;
}

static int wb_arm_pcs_first(void)
{
    if (wb.sm_diag != WB_SM_ENDPOINT || !wb.pcs_first || !wb_guard() ||
        wb.bringup.stage != GIULIA_BRINGUP_PHY_START ||
        wb.bringup.phy.state != GIULIA_PHY_IDLE || wb.bringup.pipe_checked ||
        wb.bringup.supply.power.state != GIULIA_POWER_READY ||
        wb.bringup.link.state != GIULIA_LINK_PARF_READY || !wb.bringup.link.parf_owned ||
        wb.bringup.supply.gpio.state != GIULIA_WGPIO_ON || wb_perst_held() ||
        wb_arm_early_reset())
        return -1;
    wb_log("ace5-wifi-bringup: PCS-before-PIPE reset armed before PHY access\n");
    return wb.pre_phy_clks ? wb_pre_phy_clock_hold() : 0;
}

static const struct giulia_cesta_io wb_cesta_table_io = {
    .read = wb_cesta_table_read, .write = wb_cesta_table_write,
    .barrier = wb_barrier, .validate_lease = wb_cesta_table_lease,
    .validate_reset = wb_arm_cold_reset
};

static int wb_cesta_read_cold(void)
{
    const struct giulia_cesta_io io = {
        .read = wb_cesta_table_read, .validate_lease = wb_cesta_table_lease
    };
    int rc = giulia_cesta_check_cold(&io);
    if (rc == GIULIA_CESTA_OK && wb_cesta_table_lease(0))
        rc = GIULIA_CESTA_LEASE;
    wb_log("ace5-wifi-bringup: cesta cold-check rc=%d\n", rc);
    return rc;
}

static int wb_cesta_observe(void)
{
    /* OnePlus pineapple-pcie DT pcie-sm-debug offsets (sequence state). */
    static const uint32_t offsets[] = {0x1090u, 0x1088u, 0x1070u, 0x1078u, 0x1080u, 0x109cu};
    for (unsigned sample = 0; sample < 2; sample++) {
        uint32_t values[sizeof(offsets) / sizeof(offsets[0])];
        for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
            if (wb_cesta_table_lease(0) ||
                wb_cesta_table_read(0, offsets[i], &values[i]) || values[i] == UINT32_MAX)
                return GIULIA_CESTA_IO;
        }
        wb_log("ace5-wifi-bringup: cesta execution sample=%u start=%#x pc=%#x power=%#x wait=%#x branch=%#x clkreq=%#x\n",
            sample, values[0], values[1], values[2], values[3], values[4], values[5]);
        if (sample == 0) delay(5000);
    }
    return GIULIA_CESTA_OK;
}

struct wb_gpio_sample {
    int rc;
    struct giulia_wifi_gpio_signals pins;
};

static void wb_log_gpio_sample(const char *phase, const struct wb_gpio_sample *sample)
{
    if (sample->rc) {
        wb_log("ace5-wifi-bringup: endpoint pins %s rc=%d\n", phase, sample->rc);
        return;
    }
    const struct giulia_wifi_gpio_signals *p = &sample->pins;
    wb_log("ace5-wifi-bringup: endpoint pins %s en=%#x sw=%#x wlan_sw=%#x perst=%#x clkreq=%#x wake=%#x\n",
        phase, p->wlan_en, p->sw_ctrl, p->wlan_sw_ctrl, p->perst, p->clkreq, p->wake);
}

/* TLMM IO bit 0 is the sampled input and bit 1 is the output latch.
 * The latch can change without the line following it. Gate LTSSM on both
 * observations, using the samples already taken for diagnostics. */
static int wb_endpoint_pin_gate(const struct wb_gpio_sample *sample, uint32_t perst)
{
    if (sample->rc) return GIULIA_LINK_READBACK;
    if ((sample->pins.wlan_en & 3u) != 3u || (sample->pins.perst & 3u) != perst)
        return GIULIA_LINK_PREREQUISITE;
    return GIULIA_LINK_OK;
}

static int wb_endpoint_transaction(void)
{
    struct giulia_pcie_link *link = &wb.bringup.link;
    struct giulia_wifi_gpio *gpio = &wb.bringup.supply.gpio;
    uint32_t flags = GIULIA_LINK_PREPARE_REQUIRED |
        GIULIA_LINK_EXACT_ROOT_ID | GIULIA_LINK_VENDOR_TRAINING;
    if (wb.native_width) flags |= GIULIA_LINK_NATIVE_WIDTH;
    if (wb.stock_train) flags |= GIULIA_LINK_STOCK_TRAIN_LANES;
    if (wb.x1_id) flags |= GIULIA_LINK_NATIVE_X1_ID;
    if (wb.keep_sls) flags |= GIULIA_LINK_PRESERVE_SPEED_CAP;
    int rc = giulia_pcie_link_prepare(link, flags);
    wb_log("ace5-wifi-bringup: DBI root id rc=%d id=%#x; prepare stage=%u fail=%u last=%u/%#x/%#x\n",
        rc, link->root_id, link->stage, link->failure_stage, wb_dbg_r32[0], wb_dbg_r32[1], wb_dbg_r32[2]);
    if (rc) return rc;
    if (wb_stage4_reset_lease() || wb.quiet->reset_ack != LOADER_QUIET_RESET_ARMED)
        return GIULIA_LINK_PREREQUISITE;

    /* prepare verifies the capability and target; never report merely
     * because the option was present, or after the reset lease was lost. */
    if (wb.keep_sls) {
        if (!link->preserve_speed_cap) return GIULIA_LINK_PROFILE;
        wb_log("ace5-wifi-bringup: native speed cap=3 target=1\n");
    }
    if (wb.native_width)
        wb_log("ace5-wifi-bringup: native train lanes snapshot=%u\n", (unsigned)link->snapshot_lanes);
    struct wb_gpio_sample before, released, trained;
    before.rc = giulia_wifi_gpio_snapshot(gpio, &before.pins);
    int attempted = 0, sampled_release = 0, released_ok = 0, release_gate_error = 0;
    wb.endpoint_reset_unconfirmed = 1;
    rc = wb_endpoint_pin_gate(&before, 0u);
    if (rc) {
        wb_log("ace5-wifi-bringup: endpoint pin gate refused held rc=%d; training skipped\n", rc);
    } else if (!wb_endpoint_io_ok()) {
        rc = GIULIA_LINK_PREREQUISITE;
    } else {
        int release = giulia_wifi_gpio_perst_release(gpio,
            GIULIA_WGPIO_RELEASE_REQUIRED | GIULIA_WGPIO_BT_CONTROL_EXCLUSIVE);
        if (release)
            wb_log("ace5-wifi-bringup: PCIe endpoint reset release rc=%d\n", release);
        else
            released_ok = 1; /* avoid synchronous loader/console output before LTSSM */
        rc = GIULIA_LINK_PREREQUISITE;
        if (!release && wb_guard() && wb.quiet->reset_ack == LOADER_QUIET_RESET_ARMED) {
            sampled_release = 1;
            released.rc = giulia_wifi_gpio_snapshot(gpio, &released.pins);
            rc = wb_endpoint_pin_gate(&released, 3u);
            release_gate_error = rc;
        }
    }
    if (!rc && !wb_endpoint_io_ok()) rc = GIULIA_LINK_PREREQUISITE;
    if (!rc) {
        attempted = 1;
        rc = giulia_pcie_link_identify(link, GIULIA_LINK_TRAIN_REQUIRED);
        /* Sample the released pins now, not after driving PERST# low. */
        trained.rc = wb_guard() ? giulia_wifi_gpio_snapshot(gpio, &trained.pins) : GIULIA_WGPIO_PREREQUISITE;
    }
    /* Preserve the physical event order and all ownership/readback gates,
     * not the synchronous print latency. When the lease is lost, no further
     * device read/write is legal; only previously cached evidence is logged. */
    int can_reassert = wb_guard();
    int held = GIULIA_WGPIO_PREREQUISITE;
    if (can_reassert) {
        held = giulia_wifi_gpio_perst_assert(gpio,
            GIULIA_WGPIO_BEGIN_REQUIRED | GIULIA_WGPIO_BT_CONTROL_EXCLUSIVE);
        if (!held && (wb_gpio_r(0, 94u * 0x1000u + 4u) & 3u))
            held = GIULIA_WGPIO_READBACK;
        if (!held && wb_endpoint_io_ok()) wb.endpoint_reset_unconfirmed = 0;
        if (wb.guard_failed && !held) held = GIULIA_WGPIO_PREREQUISITE;
    }
    if (wb.guard_failed && !rc) rc = GIULIA_LINK_PREREQUISITE;
    /* Report in the historical event order. An enable-bit readback is NOT
     * proof of Link Up; an attempted or skipped train alone proves neither. */
    if (released_ok)
        wb_log("ace5-wifi-bringup: PCIe endpoint reset release rc=0 (deferred; ltssm_enable_readback=%u)\n",
            (unsigned)(attempted && (link->train_parf_valid & GIULIA_LINK_TRAIN_PARF_ENABLE_VALID)));
    if (release_gate_error)
        wb_log("ace5-wifi-bringup: endpoint pin gate refused released rc=%d; training skipped\n", release_gate_error);
    if (attempted) {
        /* Field evidence only, not proof of an endpoint. Require at least
         * one real LTSSM sample even when the later link check times out. */
        if (wb.stock_train && link->train_lanes_verified && link->train_samples)
            wb_log("ace5-wifi-bringup: stock train lanes before=%u after=%u\n",
                (unsigned)link->train_lanes_before, (unsigned)link->train_lanes_after);
        wb_log("ace5-wifi-bringup: PCIe endpoint identify rc=%d id=%#x lnk=%#x stage=%u fail=%u polls=%u debug=%#x/%#x elbi=%#x ltssm=%#x\n",
            rc, link->endpoint_id, link->link_status, link->stage, link->failure_stage,
            link->polls, link->debug0, link->debug1, link->elbi_status,
            (link->elbi_status >> 12) & 0x3fu);
    }
    if (!can_reassert)
        return GIULIA_LINK_PREREQUISITE;
    wb_log("ace5-wifi-bringup: PCIe endpoint reset reassert rc=%d\n", held);
    wb_log_gpio_sample("held", &before);
    if (attempted) {
        wb_log("ace5-wifi-bringup: PCIe training samples=%u first=%u last=%u seen_lo=%#x seen_hi=%#x\n",
            link->train_samples, link->train_ltssm_first, link->train_ltssm_last,
            link->train_ltssm_seen[0], link->train_ltssm_seen[1]);
        /* Cached samples from identify; no MMIO, even after cleanup. */
        wb_log("ace5-wifi-bringup: train evidence parf_valid=%#x parf_enable=%#x parf_timeout=%#x dllla=%u lt=%u xmlh=%u ready=%u\n",
            link->train_parf_valid, link->train_parf_enable_raw, link->train_parf_timeout_raw,
            link->train_dllla_samples, link->train_lt_samples,
            link->train_xmlh_samples, link->train_ready_samples);
    }
    if (sampled_release) wb_log_gpio_sample("released", &released);
    if (attempted) wb_log_gpio_sample("after-training", &trained);
    if (wb.x1_id && attempted && !rc && !held)
        wb_log("ace5-wifi-bringup: native endpoint width=%u\n", (unsigned)link->id_lanes);
    return held ? held : rc;
}

static int wb_pcie_identify_endpoint(void)
{
    if (wb.sm_diag != WB_SM_ENDPOINT || wb.endpoint_io_window)
        return GIULIA_LINK_PREREQUISITE;
    wb.endpoint_io_window = 1;
    int rc = wb_endpoint_io_ok() ? wb_endpoint_transaction() : GIULIA_LINK_PREREQUISITE;
    if (!wb_endpoint_io_ok()) {
        /* Preserve a completed CFG ID as evidence, but never advertise a
         * newly identified live endpoint after the board lease was lost. No
         * core cleanup here: it would touch hardware we no longer own. */
        struct giulia_pcie_link *link = &wb.bringup.link;
        if (link->state == GIULIA_LINK_IDENTIFIED) {
            link->failure_stage = link->stage;
            link->error = GIULIA_LINK_PREREQUISITE;
            link->state = GIULIA_LINK_FAULT;
        }
        wb_log("ace5-wifi-bringup: endpoint lease lost; retained until reset, prior ID is a sample only\n");
        rc = GIULIA_LINK_PREREQUISITE;
    }
    wb.endpoint_io_window = 0;
    return rc;
}

static int wb_cesta_pre_dbi(void)
{
    uint32_t pipe = 0;
    if (wb_stage4_reset_lease() || wb.quiet->reset_ack != LOADER_QUIET_RESET_ARMED ||
        !wb.crm_c.address || wb_verify_noc(0) ||
        giulia_pcie_power_pipe_ready(&wb.bringup.supply.power, &pipe))
        return GIULIA_LINK_PREREQUISITE;

    /* crm.c's PCIe VCD0 crm_c window, distinct from the CRM HW DRV's PWR_IDX_STATUS. */
    wb_log("ace5-wifi-bringup: CESTA perf read begin\n");
    uint32_t current = wb_mmio(&wb.crm_c, 0xcu);
    uint32_t aggregate = wb_mmio(&wb.crm_c, 0x4u);
    uint32_t sequence = wb_mmio(&wb.crm_c, 0x1a0u);
    const uint32_t keep_clk = (1u << 6) | (1u << 1);
    uint32_t sys = wb_mmio(&wb.parf, 0);
    if (current == UINT32_MAX || aggregate == UINT32_MAX ||
        sequence == UINT32_MAX || sys == UINT32_MAX)
        return GIULIA_LINK_READBACK;
    wb_log("ace5-wifi-bringup: CESTA perf current=%#x aggregate=%#x sequence=%#x sys=%#x pipe=%#x\n",
        current, aggregate, sequence, sys, pipe);
    if (wb.sm_diag == WB_SM_PERF)
        return GIULIA_LINK_OK;
    uint32_t requested = wb.stock_perf ? GIULIA_PCIE_STOCK_STARTUP_PERF : GIULIA_PCIE_BOOT_PERF;
    if (current != requested || (wb.stock_perf && aggregate != requested) || !wb.dbi.address ||
        (sys & ~keep_clk) != (0x365eu & ~keep_clk)) {
        wb_log("ace5-wifi-bringup: DBI admission refused; PERF or PARF_SYS differs\n");
        return GIULIA_LINK_PREREQUISITE;
    }
    if (wb_stage4_reset_lease() || wb.quiet->reset_ack != LOADER_QUIET_RESET_ARMED)
        return GIULIA_LINK_PREREQUISITE;
    /* Match the vendor PARF_SYS_CTRL=0x365e clock-removal bits, and preserve
     * every unrelated bit. CESTA already requires a reset for teardown. */
    if ((sys & keep_clk) != keep_clk) {
        wb_store(&wb.parf, 0, sys | keep_clk);
        if (wb_mmio(&wb.parf, 0) != (sys | keep_clk))
            return GIULIA_LINK_READBACK;
    }
    wb_log("ace5-wifi-bringup: DBI clock guard sys=%#x\n", sys | keep_clk);
    if (wb.sm_diag == WB_SM_ENDPOINT)
        return wb_pcie_identify_endpoint();
    int rc = giulia_pcie_link_read_root_id(&wb.bringup.link, GIULIA_LINK_PREPARE_REQUIRED);
    wb_log("ace5-wifi-bringup: DBI root id rc=%d id=%#x\n", rc, wb.bringup.link.root_id);
    return rc;
}

static int wb_cesta_stage_and_discard(void)
{
    struct giulia_cesta sm = {0};
    const uint32_t sequence_bytes = 4u * GIULIA_CESTA_SEQUENCE_WORDS;
    int staged = giulia_cesta_stage(&sm, &wb_cesta_table_io, wb.cesta_profile, sequence_bytes,
        wb.cesta_profile + sequence_bytes, WB_SM_PROFILE_BYTES - sequence_bytes,
        GIULIA_CESTA_BRANCH_OFFSET, GIULIA_CESTA_START_OFFSET);
    if (staged == GIULIA_CESTA_OK)
        staged = giulia_cesta_verify_staged(&sm);
    if (staged == GIULIA_CESTA_OK && wb.sm_diag >= WB_SM_START) {
        int start = giulia_cesta_start(&sm);
        if (sm.state == GIULIA_CESTA_RESET_REQUIRED) {
            int observed = start == GIULIA_CESTA_RETAINED ? wb_cesta_observe() : start;
            wb_log("ace5-wifi-bringup: cesta START result=%d observe=%d; retained until reset\n",
                start, observed);
            if (observed == GIULIA_CESTA_OK && wb.sm_diag >= WB_SM_PERF) {
                int rc = wb_cesta_pre_dbi();
                wb_log("ace5-wifi-bringup: CESTA follow-up rc=%d\n", rc);
            }
            wb.guard_failed = 1;
            if (!wb.endpoint_reset_unconfirmed)
                loader_return();
            return GIULIA_CESTA_RETAINED;
        }
        staged = start;
    }
    int write_attempted = sm.dirty != 0;
    int discarded = giulia_cesta_discard(&sm);
    int cold = discarded == GIULIA_CESTA_OK ? wb_cesta_read_cold() : GIULIA_CESTA_STATE;
    wb_log("ace5-wifi-bringup: cesta stage rc=%d discard=%d cold=%d\n", staged, discarded, cold);
    if (discarded != GIULIA_CESTA_OK || (write_attempted && cold != GIULIA_CESTA_OK))
        wb.guard_failed = 1;
    return staged != GIULIA_CESTA_OK ? staged : cold;
}

static int wb_cesta_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int wb_load_cesta_profile(void)
{
    static const uint8_t expected[] = {
        0xb3, 0xed, 0x15, 0x4a, 0x6c, 0xab, 0xba, 0x92,
        0x1d, 0x99, 0xcf, 0x70, 0x3d, 0x70, 0x25, 0x5e,
        0x22, 0x39, 0x97, 0x76, 0x83, 0x15, 0x39, 0x92,
        0xc4, 0xa8, 0xf1, 0xcc, 0x52, 0x11, 0xd9, 0x0e
    };
    char encoded[2u * WB_SM_PROFILE_BYTES + 3u] = {0};
    if (!PE_parse_boot_argn("ace5_cesta_profile", encoded, sizeof(encoded)) ||
        encoded[0] != 'v' || encoded[2u * WB_SM_PROFILE_BYTES + 1u] != '\0')
        return -1;
    for (unsigned i = 0; i < WB_SM_PROFILE_BYTES; i++) {
        int high = wb_cesta_nibble(encoded[1u + 2u * i]);
        int low = wb_cesta_nibble(encoded[2u + 2u * i]);
        if (high < 0 || low < 0) return -1;
        wb.cesta_profile[i] = (uint8_t)((high << 4) | low);
    }
    uint8_t digest[sizeof(expected)];
    SHA256_CTX hash;
    SHA256_Init(&hash);
    SHA256_Update(&hash, wb.cesta_profile, sizeof(wb.cesta_profile));
    SHA256_Final(digest, &hash);
    return __builtin_memcmp(digest, expected, sizeof(expected)) == 0 ? 0 : -1;
}

static uint64_t wb_watchdog_raw(uint64_t id, uint64_t a1, uint64_t a2,
    uint64_t *r1, uint64_t *r2)
{
#ifdef GIULIA_WIFI_BRINGUP_XNU_HOST_TEST
    return wb_test_watchdog_call(id, a1, a2, r1, r2);
#else
    register uint64_t x0 __asm__("x0") = id;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = 0;
    __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :
        : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
          "x14", "x15", "x16", "x17", "memory");
    if (r1) *r1 = x1;
    if (r2) *r2 = x2;
    return x0;
#endif
}

static uint64_t wb_watchdog_call(uint64_t id, uint64_t a1, uint64_t a2,
    uint64_t *r1, uint64_t *r2)
{
    return giulia_wdt_call32(wb_watchdog_raw, id, a1, a2, r1, r2);
}

static int wb_watchdog_cancel(void)
{
    uint64_t ctl = 0;
    if (wb_watchdog_call(GIULIA_WDT_STATUS, 0, 0, &ctl, 0))
        return -1;
    if (ctl & GIULIA_WDT_ENABLED) {
        if (wb_watchdog_call(GIULIA_WDT_CONTROL, 2, 0, 0, 0) ||
            wb_watchdog_call(GIULIA_WDT_STATUS, 0, 0, &ctl, 0) ||
            (ctl & GIULIA_WDT_ENABLED))
            return -1;
    }
    return 0;
}

static int wb_watchdog_quiet_test(void)
{
    if (wb_stage4_reset_lease())
        return -1;
    uint64_t ctl = 0, since = 0;
    uint64_t ret = wb_watchdog_call(GIULIA_WDT_STATUS, 0, 0, &ctl, &since);
    wb_log("ace5-wifi-bringup: quiet watchdog status ret=%llu ctl=%#llx since=%#llx\n",
        (unsigned long long)ret, (unsigned long long)ctl, (unsigned long long)since);
    if (ret || (ctl & (GIULIA_WDT_ENABLED | GIULIA_WDT_EXPIRED)))
        return -1;
    if (wb.watchdog_trial == WB_WATCHDOG_STATUS)
        return 0;

    uint64_t detail;
    int owned = 0;
    const char *failure = giulia_wdt_configure_owned(wb_watchdog_call, &detail, &owned);
    if (failure) {
        wb_log("ace5-wifi-bringup: quiet watchdog arm failed %s=%#llx\n",
            failure, (unsigned long long)detail);
    } else {
        wb_log("ace5-wifi-bringup: quiet watchdog armed; observing expiry\n");
        for (unsigned second = 0; second < WB_WATCHDOG_WAIT_SECONDS; ++second) {
            delay(1000000);
            if (wb_stage4_reset_lease()) break;
        }
        wb_log("ace5-wifi-bringup: quiet watchdog expiry not observed\n");
    }
    /* A rejected timing or ENABLE call does not grant the right to disable. */
    if (owned && wb_watchdog_cancel()) {
        wb.guard_failed = 1;
        wb_log("ace5-wifi-bringup: quiet watchdog disarm unconfirmed\n");
    }
    return -1; /* A successful expiry resets the device, so it cannot return here. */
}

static void wb_thread(void *arg, __unused wait_result_t wait)
{
    uint64_t pa = (uint64_t)(uintptr_t)arg;
    struct giulia_wifi_bringup_config cfg = {0};
    struct giulia_wlan_rail_baseline unk_rails[7] = {0};
    int rc = -1, stop_rc = -1, quiet = 0;
    int link_access = wb.sm_diag < WB_SM_TABLES && !wb.watchdog_trial && !wb.cold_reset;

    wb_map(&wb.shared, pa, sizeof(struct loader_firmware_quiet), VM_WIMG_WCOMB);
    if (!wb.shared.address) goto done;
    wb.quiet = (volatile struct loader_firmware_quiet *)wb.shared.address;
    if (wb.quiet->magic != LOADER_QUIET_MAGIC || wb.quiet->version != LOADER_QUIET_VERSION ||
        wb.quiet->size != sizeof(*wb.quiet) || wb.quiet->request || wb.quiet->ack || wb.quiet->active)
        goto done;

    wb_map(&wb.gcc, GIULIA_QOS_GCC_PHYS, GIULIA_QOS_GCC_SIZE, VM_WIMG_IO);
    wb_map(&wb.tcsr, GIULIA_POWER_TCSR_PHYS, 0x40000u, VM_WIMG_IO);
    wb_map(&wb.tlmm, GIULIA_WIFI_GPIO_PA, GIULIA_WIFI_GPIO_SIZE, VM_WIMG_IO);
    wb_map(&wb.rsc, 0x17a20000u, 0x10000u, VM_WIMG_IO);
    wb_map(&wb.db, 0x81c60000u, GIULIA_RPMH_DB_MAX, VM_WIMG_WCOMB);
    wb_map(&wb.gic, 0x17100000u, 0x10000u, VM_WIMG_IO);
    wb_map(&wb.msg, GIULIA_AOP_MSGRAM_PHYS, GIULIA_AOP_MSGRAM_SIZE, VM_WIMG_IO);
    wb_map(&wb.ipcc, 0x00406000u, 0x1000u, VM_WIMG_IO);
    wb_map(&wb.parf, 0x01c00000u, 0x3000u, VM_WIMG_IO);
    wb_map(&wb.phy, 0x01c06000u, 0x2000u, VM_WIMG_IO);
    if (link_access) {
        wb_map(&wb.dbi, GIULIA_PCIE_LINK_DBI_PA, 0x1000u, VM_WIMG_IO);
        wb_map(&wb.atu, GIULIA_PCIE_LINK_ATU_PA, 0x1000u, VM_WIMG_IO);
        wb_map(&wb.cfg, GIULIA_PCIE_LINK_CFG_PA, GIULIA_PCIE_LINK_CFG_SIZE, VM_WIMG_IO);
        wb_map(&wb.bar0, GIULIA_PCIE_LINK_BAR0_PA, GIULIA_PCIE_LINK_BAR0_SIZE, VM_WIMG_IO);
    } else if (wb.sm_diag >= WB_SM_DBI_ID) {
        wb_map(&wb.dbi, GIULIA_PCIE_LINK_DBI_PA, 0x1000u, VM_WIMG_IO);
        if (wb.sm_diag == WB_SM_ENDPOINT) {
            wb_map(&wb.atu, GIULIA_PCIE_LINK_ATU_PA, 0x1000u, VM_WIMG_IO);
            wb_map(&wb.cfg, GIULIA_PCIE_LINK_CFG_PA, GIULIA_PCIE_LINK_CFG_SIZE, VM_WIMG_IO);
        }
    }
    wb_map(&wb.crm, GIULIA_NOC_CRM_PHYS, GIULIA_NOC_CRM_SIZE, VM_WIMG_IO);
    if (wb.sm_diag >= WB_SM_PERF)
        wb_map(&wb.crm_c, WB_CRM_C_PHYS, WB_CRM_C_SIZE, VM_WIMG_IO);
    wb_map(&wb.qos_pcie, GIULIA_QOS_PCIE_PHYS, GIULIA_QOS_PCIE_SIZE, VM_WIMG_IO);
    wb_map(&wb.qos_gem, GIULIA_QOS_GEM_PHYS, GIULIA_QOS_GEM_SIZE, VM_WIMG_IO);
    if (wb.sm_diag)
        wb_map(&wb.pcie_sm, GIULIA_NOC_SM_PHYS, GIULIA_NOC_SM_SIZE, VM_WIMG_IO);
    if (!wb.gcc.address || !wb.tcsr.address || !wb.tlmm.address || !wb.rsc.address ||
        !wb.db.address || !wb.gic.address || !wb.msg.address || !wb.ipcc.address ||
        !wb.parf.address || !wb.phy.address || !wb.crm.address ||
        (link_access && (!wb.dbi.address || !wb.atu.address ||
        !wb.cfg.address || !wb.bar0.address)) ||
        (wb.sm_diag >= WB_SM_DBI_ID && !wb.dbi.address) ||
        (wb.sm_diag == WB_SM_ENDPOINT && (!wb.atu.address || !wb.cfg.address)) ||
        (wb.sm_diag >= WB_SM_PERF && !wb.crm_c.address) ||
        !wb.qos_pcie.address || !wb.qos_gem.address ||
        (wb.sm_diag && !wb.pcie_sm.address)) {
        wb_log("ace5-wifi-bringup: MMIO mapping failed\n");
        goto done;
    }

    wb.db_version = *(volatile uint32_t *)wb.db.address;
    quiet = 1;
    if (!wb_quiet_req(1) || !wb_irq_acquire()) {
        wb_log("ace5-wifi-bringup: quiet/IRQ37 acquire failed\n");
        goto release;
    }

    cfg.supply.gpio = (struct giulia_wifi_gpio_io){0, wb_gpio_r, wb_gpio_w, wb_barrier, wb_delay_u};
    cfg.supply.aop = (struct giulia_aop_io){0, wb_aop_r, wb_aop_w, wb_barrier, wb_delay_u, wb_aop_kick};
    cfg.supply.rpmh = (struct giulia_rpmh_io){0, wb_rpmh_r, wb_rpmh_w, wb_barrier, wb_delay_u};
    cfg.supply.power = (struct giulia_pcie_power_io){0, wb_pwr_r, wb_pwr_w, wb_barrier, wb_delay_u};
    cfg.supply.db = (const uint8_t *)wb.db.address;
    cfg.supply.db_bytes = GIULIA_RPMH_DB_MAX;
    cfg.supply.db_version = wb.db_version;
    cfg.supply.pdc_table = wb.pdc_table;
    cfg.supply.pdc_bytes = sizeof(wb.pdc_table);
    uint32_t votes0 = wb_pwr_r(0, GIULIA_POWER_GCC, 0x52000);
    uint32_t votes1 = wb_pwr_r(0, GIULIA_POWER_GCC, 0x52008);
    uint32_t core_gdsc = wb_pwr_r(0, GIULIA_POWER_GCC, 0x6b004);
    uint32_t phy_gdsc = wb_pwr_r(0, GIULIA_POWER_GCC, 0x6c000);
    cfg.supply.rail_baseline = unk_rails;
    cfg.supply.conditions = GIULIA_SUPPLY_REQUIRED |
        ((votes0 & (1u << 22)) ? GIULIA_SUPPLY_RCHNG_HANDOFF : 0);
    cfg.supply.aop_policy = GIULIA_AOP_OPEN_ON_INHERITED_LINK;
    cfg.supply.pdc_policy = GIULIA_WLAN_PDC_KEEP_TABLE;
    cfg.supply.rail_policy = GIULIA_WLAN_KEEP_VOTES;
    cfg.supply.allow_retained_cpu_floor = 1;
    cfg.supply.validate_external = wb_ext_supply;

    cfg.link = (struct giulia_pcie_link_io){0, wb_link_r32, wb_link_w32, wb_link_r16, wb_link_w16, wb_barrier, wb_delay};
    cfg.phy = (struct giulia_pcie_phy_io){0, wb_phy_r, wb_phy_w, wb_barrier, wb_delay, 0, 0};
    cfg.bar = (struct giulia_kiwi_bar_io){0, wb_bar_r32, wb_bar_w32, GIULIA_PCIE_LINK_BAR0_SIZE};
    cfg.mhi = (struct giulia_mhi_snapshot_io){0, wb_bar_r32, GIULIA_PCIE_LINK_BAR0_SIZE};
    cfg.prepare_noc = wb_prepare_noc;
    cfg.release_noc = wb_release_noc;
    cfg.verify_noc = wb_verify_noc;
    if (wb.pcs_first) cfg.arm_pcs_first = wb_arm_pcs_first;
    cfg.max_stage = wb.max_stage;

    rc = giulia_wifi_bringup_start(&wb.bringup, &cfg);
    wb_log("ace5-wifi-bringup: start rc=%d stage=%u max=%u failed=%u err=%d sub=%d cleanup=%d\n",
        rc, wb.bringup.stage, wb.max_stage, wb.bringup.failed_stage, wb.bringup.error,
        wb.bringup.sub_error, wb.bringup.cleanup_error);
    wb_log("ace5-wifi-bringup: noc step=%u rc=%d crm=%d arc=%d gem=%d qos=%d(%u,%#x,%#x)\n",
        wb.noc_step, wb.noc_rc, wb.crm_rc, wb.arc_rc, wb.gem_rc, wb.qos_rc,
        wb.qos.failed_block, wb.qos.failed_offset, wb.qos.observed);
    wb_log("ace5-wifi-bringup: supply state=%u failed=%u err=%d drv=%d retained=%#x pdc=%u/%#x rails=%u/%#x\n",
        wb.bringup.supply.state, wb.bringup.supply.failed_stage,
        wb.bringup.supply.error, wb.bringup.supply.driver_error,
        wb.bringup.supply.retained,
        wb.bringup.supply.pdc.state, wb.bringup.supply.pdc.delivered,
        wb.bringup.supply.rails.state, wb.bringup.supply.rails.completed_mask);
    wb_log("ace5-wifi-bringup: gcc votes0=%#x votes1=%#x core_gdsc=%#x phy_gdsc=%#x\n",
        votes0, votes1, core_gdsc, phy_gdsc);
    wb_log("ace5-wifi-bringup: link stage=%u fail=%u err=%d r32=(%u,%#x,%#x) r16=(%u,%#x,%#x)\n",
        wb.bringup.link.stage, wb.bringup.link.failure_stage, wb.bringup.link.error,
        wb_dbg_r32[0], wb_dbg_r32[1], wb_dbg_r32[2],
        wb_dbg_r16[0], wb_dbg_r16[1], wb_dbg_r16[2]);
    wb_log("ace5-wifi-bringup: root=%#x ep=%#x link_sts=%#x bar0=%#x/%#x\n",
        wb.bringup.link.root_id, wb.bringup.link.endpoint_id,
        wb.bringup.link.link_status, wb.bringup.link.bar0_mask, wb.bringup.link.bar0_mask_hi);
    if (wb.bringup.pipe_checked)
        wb_log("ace5-wifi-bringup: post-phy pipe_cbcr=%#x\n", wb.bringup.pipe_cbcr);
    if (rc == GIULIA_BRINGUP_OK) {
        if (wb.cold_reset && !wb.sm_diag) {
            if (wb_arm_cold_reset(0) == 0) {
                /* The loader, not this core, must reset the phone for this trial. */
                for (unsigned second = 0; second < 30; second++) delay(1000000);
                wb_log("ace5-wifi-bringup: loader cold reset not observed\n");
                wb.guard_failed = 1;
                loader_return();
            }
            if (wb.guard_failed) goto release;
            goto stop;
        }
        if (wb.watchdog_trial) {
            int wdt_rc = wb_watchdog_quiet_test();
            if (wb.guard_failed) goto release;
            if (wdt_rc) goto stop;
        }
        if (wb.sm_diag >= WB_SM_TABLES) {
            int sm_rc = wb.sm_diag >= WB_SM_STAGE ? wb_cesta_stage_and_discard() : wb_cesta_read_cold();
            if (wb.guard_failed) goto release;
            if (sm_rc != GIULIA_CESTA_OK) goto stop;
        }
        wb_log("ace5-wifi-bringup: requested stage complete; stage=%u max=%u\n",
            wb.bringup.stage, wb.max_stage);
        if (wb.bringup.stage == GIULIA_BRINGUP_READY) {
            wb_log("ace5-wifi-bringup: soc raw=%#x fam=%u dev=%#x maj=%u min=%u fw=%s mhi_valid=%#x\n",
                wb.bringup.soc.raw, wb.bringup.soc.family, wb.bringup.soc.device,
                wb.bringup.soc.major, wb.bringup.soc.minor,
                giulia_kiwi_fw_name(&wb.bringup.soc), wb.bringup.mhi.valid);
        }
stop:
        stop_rc = giulia_wifi_bringup_stop(&wb.bringup);
        wb_log("ace5-wifi-bringup: stop rc=%d stage=%u\n", stop_rc, wb.bringup.stage);
    }

release:
    if (wb.early_reset_armed) {
        wb.guard_failed = 1;
        if (!wb.endpoint_reset_unconfirmed)
            loader_return();
        goto retained;
    }
    if (wb.guard_failed || wb.bringup.cleanup_error ||
        wb.bringup.stage == GIULIA_BRINGUP_FAULT) {
        wb_log("ace5-wifi-bringup: cleanup FAILED; resources/IRQ37/firmware quiet retained until cold reboot\n");
        goto retained;
    }
    if (wb.bringup.stage == GIULIA_BRINGUP_HELD) {
        wb_log("ace5-wifi-bringup: resources retained; firmware quiet and IRQ37 isolation retained until cold reboot\n");
        goto retained;
    }
    if (!wb_irq_release() || (quiet && !wb_quiet_req(0))) {
        wb_log("ace5-wifi-bringup: cleanup FAILED; IRQ/firmware restoration unconfirmed, mappings retained until cold reboot\n");
        goto retained;
    }
    wb_log("ace5-wifi-bringup: cleanup complete; IRQ37 restored and firmware resumed (cold reboot required)\n");
done:
    wb_unmap(&wb.pcie_sm); wb_unmap(&wb.qos_gem); wb_unmap(&wb.qos_pcie); wb_unmap(&wb.crm_c); wb_unmap(&wb.crm);
    wb_unmap(&wb.bar0); wb_unmap(&wb.cfg); wb_unmap(&wb.atu); wb_unmap(&wb.dbi);
    wb_unmap(&wb.phy); wb_unmap(&wb.parf); wb_unmap(&wb.ipcc); wb_unmap(&wb.msg);
    wb_unmap(&wb.gic); wb_unmap(&wb.db); wb_unmap(&wb.rsc); wb_unmap(&wb.tlmm);
    wb_unmap(&wb.tcsr); wb_unmap(&wb.gcc); wb_unmap(&wb.shared);
retained:
    thread_terminate(current_thread());
}

void giulia_wifi_bringup_init(void)
{
    uint32_t enabled = 0, cpus = 0, cpu = 0, seconds = 0;
    DTEntry entry;
    const void *property;
    unsigned size;
    uint64_t pa;
    thread_t thread;
    if (!PE_parse_boot_argn("ace5_wifi_bringup", &enabled, sizeof(enabled)) || enabled != 1) return;
    if (hackios_platform() != HACKIOS_PLATFORM_GIULIA) return;
    if (__atomic_load_n(&wb_started, __ATOMIC_ACQUIRE)) return;
    PE_parse_boot_argn("cpus", &cpus, sizeof(cpus));
    PE_parse_boot_argn("bootcpu", &cpu, sizeof(cpu));
    PE_parse_boot_argn("loader_return_after", &seconds, sizeof(seconds));
    PE_parse_boot_argn("ace5_wifi_bringup_stage", &wb.max_stage, sizeof(wb.max_stage));
    PE_parse_boot_argn("ace5_wifi_crm_all_states", &wb.crm_all_states, sizeof(wb.crm_all_states));
    PE_parse_boot_argn("ace5_wifi_sm_diag", &wb.sm_diag, sizeof(wb.sm_diag));
    PE_parse_boot_argn("ace5_wifi_watchdog", &wb.watchdog_trial, sizeof(wb.watchdog_trial));
    PE_parse_boot_argn("ace5_wifi_cold_reset", &wb.cold_reset, sizeof(wb.cold_reset));
    PE_parse_boot_argn("ace5_wifi_pcs_first", &wb.pcs_first, sizeof(wb.pcs_first));
    PE_parse_boot_argn("ace5_wifi_pre_phy_clks", &wb.pre_phy_clks, sizeof(wb.pre_phy_clks));
    PE_parse_boot_argn("ace5_wifi_native_width", &wb.native_width, sizeof(wb.native_width));
    PE_parse_boot_argn("ace5_wifi_keep_sls", &wb.keep_sls, sizeof(wb.keep_sls));
    PE_parse_boot_argn("ace5_wifi_x1_id", &wb.x1_id, sizeof(wb.x1_id));
    PE_parse_boot_argn("ace5_wifi_stock_train", &wb.stock_train, sizeof(wb.stock_train));
    PE_parse_boot_argn("ace5_wifi_stock_perf", &wb.stock_perf, sizeof(wb.stock_perf));
    if (wb.keep_sls && (wb.keep_sls != 1 || wb.pcs_first != 1 || wb.native_width != 1 ||
        wb.cold_reset != 1 || wb.max_stage != GIULIA_BRINGUP_PHY_START || wb.sm_diag != WB_SM_ENDPOINT)) {
        wb_log("ace5-wifi-bringup: keep SLS requires stage 4 mode 7 PCS-first native width and cold reset\n");
        return;
    }
    if (cpus != 1 || cpu != 1 || ml_get_topology_info()->num_cpus != 1 ||
        seconds < 10 || seconds > 120 || wb.max_stage > GIULIA_BRINGUP_MHI_SNAP ||
        wb.crm_all_states > 1 || wb.sm_diag > WB_SM_ENDPOINT || wb.watchdog_trial > WB_WATCHDOG_EXPIRY ||
        wb.cold_reset > 1 || wb.pcs_first > 1 || wb.native_width > 1 ||
        wb.stock_train > 1 || (wb.stock_train && !wb.native_width) ||
        wb.stock_perf > 1 || ((wb.pcs_first || wb.native_width || wb.stock_perf) && wb.sm_diag != WB_SM_ENDPOINT)) {
        wb_log("ace5-wifi-bringup: requires cpus=1 bootcpu=1 and finite 10..120s run\n");
        return;
    }
    if (wb.x1_id && (wb.x1_id != 1 || !wb.native_width || !wb.cold_reset ||
        wb.max_stage != GIULIA_BRINGUP_PHY_START || wb.sm_diag != WB_SM_ENDPOINT)) {
        wb_log("ace5-wifi-bringup: native x1 ID requires stage 4 mode 7 native width and cold reset\n");
        return;
    }
    if (wb.pre_phy_clks && (wb.pre_phy_clks != 1 || !wb.pcs_first ||
        !wb.cold_reset || wb.max_stage != GIULIA_BRINGUP_PHY_START || wb.sm_diag != WB_SM_ENDPOINT)) {
        wb_log("ace5-wifi-bringup: pre-PHY clock hold requires stage 4 mode 7 PCS-first cold reset\n");
        return;
    }
    if (wb.sm_diag && (wb.max_stage != GIULIA_BRINGUP_PHY_START || wb.crm_all_states)) {
        wb_log("ace5-wifi-bringup: CESTA diagnostic requires stage 4 and default CRM profile\n");
        return;
    }
    if (wb.watchdog_trial && (wb.sm_diag || wb.max_stage != GIULIA_BRINGUP_PHY_START ||
        wb.crm_all_states || (wb.watchdog_trial == WB_WATCHDOG_EXPIRY && seconds < 75))) {
        wb_log("ace5-wifi-bringup: watchdog trial requires isolated stage 4 and enough return time\n");
        return;
    }
    if ((wb.cold_reset && (wb.max_stage != GIULIA_BRINGUP_PHY_START || wb.crm_all_states ||
        wb.watchdog_trial || seconds < 60 || (wb.sm_diag && wb.sm_diag < WB_SM_START))) ||
        (wb.sm_diag >= WB_SM_START && !wb.cold_reset)) {
        wb_log("ace5-wifi-bringup: cold-reset profile requires isolated stage 4 and 60..120s\n");
        return;
    }
    if (wb.sm_diag >= WB_SM_STAGE && wb_load_cesta_profile()) {
        wb_log("ace5-wifi-bringup: CESTA board profile missing or mismatched\n");
        return;
    }
    const char *const incompatible[] = {
        "ace5_wifi_probe", "ace5_touch", "ace5_battery", "ace5_cpu_khz",
        "ace5_gpu_probe", "ace5_aop_channel_test", "ace5_crm_channel_test"
    };
    for (unsigned i = 0; i < sizeof(incompatible)/sizeof(incompatible[0]); i++) {
        uint32_t value = 0;
        if (PE_parse_boot_argn(incompatible[i], &value, sizeof(value)) && value) {
            wb_log("ace5-wifi-bringup: conflicting probe enabled; skipped\n");
            return;
        }
    }
    if (SecureDTLookupEntry(NULL, "/chosen", &entry) != kSuccess ||
        SecureDTGetProperty(entry, "loader-firmware-quiet", &property, &size) != kSuccess ||
        size != sizeof(pa)) {
        wb_log("ace5-wifi-bringup: missing loader-firmware-quiet\n");
        return;
    }
    __builtin_memcpy(&pa, property, sizeof(pa));
    if (!pa || (pa & 127u)) return;
    if (SecureDTGetProperty(entry, "loader-wlan-pdc-table", &property, &size) != kSuccess ||
        size != sizeof(wb.pdc_table)) {
        wb_log("ace5-wifi-bringup: missing loader-wlan-pdc-table\n");
        return;
    }
    __builtin_memcpy(wb.pdc_table, property, sizeof(wb.pdc_table));
    if (__atomic_exchange_n(&wb_started, 1, __ATOMIC_ACQ_REL)) return;
    if (kernel_thread_start(wb_thread, (void *)(uintptr_t)pa, &thread) == KERN_SUCCESS)
        thread_deallocate(thread);
    else wb_log("ace5-wifi-bringup: no test thread\n");
}
