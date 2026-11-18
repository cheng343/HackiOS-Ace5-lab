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

#define WB_IRQ (1u << 5)
/* Observed Ace 5 HLOS DRV2 CX handoff is selector 5 (corner 256 = Nominal).
 * Never overwrite DRV2 with LOWER (64) and undervolt VDD_CX across the SoC. */
#define WB_CX_NOMINAL_LEVEL 256u
#define WB_SM_TABLES 2u
#define WB_SM_STAGE 3u
#define WB_SM_PROFILE_BYTES (4u * GIULIA_CESTA_WORDS)

struct wb_map { uintptr_t allocation, address; vm_size_t size; };
static struct {
    struct wb_map shared, gcc, tcsr, tlmm, rsc, db, gic, msg, ipcc;
    struct wb_map parf, phy, dbi, atu, cfg, bar0;
    struct wb_map crm, qos_pcie, qos_gem, pcie_sm;
    volatile struct loader_firmware_quiet *quiet;
    struct giulia_wifi_bringup bringup;
    struct giulia_crm crm_dev;
    struct giulia_bcm_pcie0_plan crm_plan;
    struct giulia_gem_clock gem_clk;
    struct giulia_pcie_qos qos;
    uint8_t pdc_table[508];
    uint8_t cesta_profile[WB_SM_PROFILE_BYTES];
    uint32_t irq_dirty, irq_prior, guard_failed, db_version, max_stage;
    uint32_t crm_all_states, sm_diag, cx_sel_voted, cx_level_voted;
    uint32_t noc_step;
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

static int wb_guard(void)
{
    uint32_t enabled = wb_mmio(&wb.gic, 0x104), active = wb_mmio(&wb.gic, 0x304);
    return !wb.guard_failed && wb_quiet_ok() && wb.irq_dirty && enabled != UINT32_MAX &&
        active != UINT32_MAX && !((enabled | active) & WB_IRQ);
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

static uint32_t wb_gpio_r(void *c, uint32_t off) { (void)c; return wb_mmio(&wb.tlmm, off); }
static void wb_gpio_w(void *c, uint32_t off, uint32_t v) { (void)c; wb_store(&wb.tlmm, off, v); }
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
    if (r == GIULIA_LINK_DBI && off == 0)
        wb_log("ace5-wifi-bringup: DBI+0 access begin\n");
    v = wb_mmio(link_reg(r), off);
    wb_dbg_r32[0] = r; wb_dbg_r32[1] = off; wb_dbg_r32[2] = v;
    if (r == GIULIA_LINK_DBI && off == 0)
        wb_log("ace5-wifi-bringup: DBI+0 access returned %#x\n", v);
    return v;
}
static void wb_link_w32(void *c, unsigned r, uint32_t off, uint32_t v) { (void)c; wb_store(link_reg(r), off, v); }
static uint16_t wb_link_r16(void *c, unsigned r, uint32_t off)
{
    uint16_t v = *(volatile uint16_t *)(link_reg(r)->address + off);
    (void)c;
    wb_barrier(0);
    wb_dbg_r16[0] = r; wb_dbg_r16[1] = off; wb_dbg_r16[2] = v;
    return v;
}
static void wb_link_w16(void *c, unsigned r, uint32_t off, uint16_t v)
{ (void)c; *(volatile uint16_t *)(link_reg(r)->address + off) = v; wb_barrier(0); }

static uint32_t wb_phy_r(void *c, uint32_t off) { (void)c; return wb_mmio(&wb.phy, off); }
static void wb_phy_w(void *c, uint32_t off, uint32_t v) { (void)c; wb_store(&wb.phy, off, v); }
static int wb_bar_r32(void *c, uint32_t off, uint32_t *val) { (void)c; *val = wb_mmio(&wb.bar0, off); return 0; }
static int wb_bar_w32(void *c, uint32_t off, uint32_t val) { (void)c; wb_store(&wb.bar0, off, val); return 0; }

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

static int wb_cesta_table_lease(__unused void *cookie)
{
    if (!wb_guard()) {
        wb.guard_failed = 1;
        return -1;
    }
    if (!wb.pcie_sm.address || wb.bringup.stage != GIULIA_BRINGUP_PHY_START ||
        !wb.bringup.pipe_checked || wb.bringup.link.state != GIULIA_LINK_PARF_READY ||
        !wb.bringup.link.parf_owned ||
        wb.bringup.supply.gpio.state != GIULIA_WGPIO_ON)
        return -1;
    /* GPIO94 remains an AP-controlled GPIO output and drives PERST# low. */
    uint32_t cfg = wb_mmio(&wb.tlmm, 94u * 0x1000u);
    uint32_t io = wb_mmio(&wb.tlmm, 94u * 0x1000u + 4u);
    uint32_t ltssm = wb_mmio(&wb.parf, 0x1b0u);
    if ((cfg & ((1u << 12) | (1u << 9) | (15u << 2))) != (1u << 9) ||
        (io & 3u) || (ltssm & (1u << 8))) {
        wb.guard_failed = 1;
        return -1;
    }
    return 0;
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
    if (wb.sm_diag != WB_SM_STAGE || (off & 3u) ||
        (off >= 4u * GIULIA_CESTA_SEQUENCE_WORDS &&
        (off < branch || off >= branch + 4u * GIULIA_CESTA_BRANCH_WORDS)) ||
        wb_cesta_table_lease(cookie))
        return -1;
    wb_log("ace5-wifi-bringup: cesta table write begin off=%#x\n", off);
    wb_store(&wb.pcie_sm, off, value);
    return 0;
}

static const struct giulia_cesta_io wb_cesta_table_io = {
    .read = wb_cesta_table_read, .write = wb_cesta_table_write,
    .barrier = wb_barrier, .validate_lease = wb_cesta_table_lease
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

static int wb_cesta_stage_and_discard(void)
{
    struct giulia_cesta sm = {0};
    const uint32_t sequence_bytes = 4u * GIULIA_CESTA_SEQUENCE_WORDS;
    int staged = giulia_cesta_stage(&sm, &wb_cesta_table_io, wb.cesta_profile, sequence_bytes,
        wb.cesta_profile + sequence_bytes, WB_SM_PROFILE_BYTES - sequence_bytes,
        GIULIA_CESTA_BRANCH_OFFSET, GIULIA_CESTA_START_OFFSET);
    if (staged == GIULIA_CESTA_OK)
        staged = giulia_cesta_verify_staged(&sm);
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

static void wb_thread(void *arg, __unused wait_result_t wait)
{
    uint64_t pa = (uint64_t)(uintptr_t)arg;
    struct giulia_wifi_bringup_config cfg = {0};
    struct giulia_wlan_rail_baseline unk_rails[7] = {0};
    int rc = -1, stop_rc = -1, quiet = 0;

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
    if (wb.sm_diag < WB_SM_TABLES) {
        wb_map(&wb.dbi, GIULIA_PCIE_LINK_DBI_PA, 0x1000u, VM_WIMG_IO);
        wb_map(&wb.atu, GIULIA_PCIE_LINK_ATU_PA, 0x1000u, VM_WIMG_IO);
        wb_map(&wb.cfg, GIULIA_PCIE_LINK_CFG_PA, GIULIA_PCIE_LINK_CFG_SIZE, VM_WIMG_IO);
        wb_map(&wb.bar0, GIULIA_PCIE_LINK_BAR0_PA, GIULIA_PCIE_LINK_BAR0_SIZE, VM_WIMG_IO);
    }
    wb_map(&wb.crm, GIULIA_NOC_CRM_PHYS, GIULIA_NOC_CRM_SIZE, VM_WIMG_IO);
    wb_map(&wb.qos_pcie, GIULIA_QOS_PCIE_PHYS, GIULIA_QOS_PCIE_SIZE, VM_WIMG_IO);
    wb_map(&wb.qos_gem, GIULIA_QOS_GEM_PHYS, GIULIA_QOS_GEM_SIZE, VM_WIMG_IO);
    if (wb.sm_diag)
        wb_map(&wb.pcie_sm, GIULIA_NOC_SM_PHYS, GIULIA_NOC_SM_SIZE, VM_WIMG_IO);
    if (!wb.gcc.address || !wb.tcsr.address || !wb.tlmm.address || !wb.rsc.address ||
        !wb.db.address || !wb.gic.address || !wb.msg.address || !wb.ipcc.address ||
        !wb.parf.address || !wb.phy.address || !wb.crm.address ||
        (wb.sm_diag < WB_SM_TABLES && (!wb.dbi.address || !wb.atu.address ||
        !wb.cfg.address || !wb.bar0.address)) ||
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
        if (wb.sm_diag >= WB_SM_TABLES) {
            int sm_rc = wb.sm_diag == WB_SM_STAGE ? wb_cesta_stage_and_discard() : wb_cesta_read_cold();
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
    wb_unmap(&wb.pcie_sm); wb_unmap(&wb.qos_gem); wb_unmap(&wb.qos_pcie); wb_unmap(&wb.crm);
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
    if (cpus != 1 || cpu != 1 || ml_get_topology_info()->num_cpus != 1 ||
        seconds < 10 || seconds > 120 || wb.max_stage > GIULIA_BRINGUP_MHI_SNAP ||
        wb.crm_all_states > 1 || wb.sm_diag > WB_SM_STAGE) {
        wb_log("ace5-wifi-bringup: requires cpus=1 bootcpu=1 and finite 10..120s run\n");
        return;
    }
    if (wb.sm_diag && (wb.max_stage != GIULIA_BRINGUP_PHY_START || wb.crm_all_states)) {
        wb_log("ace5-wifi-bringup: CESTA diagnostic requires stage 4 and default CRM profile\n");
        return;
    }
    if (wb.sm_diag == WB_SM_STAGE && wb_load_cesta_profile()) {
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
