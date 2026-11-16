/* SPDX-License-Identifier: BSD-2-Clause */
/* Finite, single-CPU XNU bring-up diagnostic for Ace 5 Wi-Fi:
 * supply (KEEP_VOTES) -> PCIe power reset -> PARF/DBI -> QMP PHY -> PERST# ->
 * Link train -> BAR0 MEM decode -> WCN7850 SoC ID (BAR+0x224) -> MHI snapshot.
 * Explicitly gated behind ace5_wifi_bringup=1, cpus=1, bootcpu=1, finite timer,
 * loader firmware-quiet and IRQ37 isolation; cold reboot required afterwards.
 */
#include <kern/clock.h>
#include <kern/thread.h>
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

#include "giulia_wifi_bringup.h"
#include "giulia_arc.h"
#include "giulia_bcm.h"
#include "giulia_gem_clock.h"
#include "giulia_pcie_noc.h"
#include "giulia_pcie_qos.h"

extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, unsigned int);
extern void ml_io_unmap(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);
extern void loader_log_line(const char *);

#define WB_IRQ (1u << 5)

struct wb_map { uintptr_t allocation, address; vm_size_t size; };
static struct {
    struct wb_map shared, gcc, tcsr, tlmm, rsc, db, gic, msg, ipcc;
    struct wb_map parf, phy, dbi, atu, cfg, bar0;
    struct wb_map crm, qos_pcie, qos_gem;
    volatile struct loader_firmware_quiet *quiet;
    struct giulia_wifi_bringup bringup;
    struct giulia_crm crm_dev;
    struct giulia_bcm_pcie0_plan crm_plan;
    struct giulia_gem_clock gem_clk;
    struct giulia_pcie_qos qos;
    uint8_t pdc_table[508];
    uint32_t irq_dirty, irq_prior, guard_failed, db_version, max_stage;
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
    __asm__ volatile("dsb sy" ::: "memory");
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
    uint32_t v = wb_mmio(link_reg(r), off);
    (void)c;
    wb_dbg_r32[0] = r; wb_dbg_r32[1] = off; wb_dbg_r32[2] = v;
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
    int rc = 0, r;
    if ((wb.qos.state == GIULIA_QOS_ACTIVE || wb.qos.state == GIULIA_QOS_FAULT) &&
        (wb_mmio(&wb.gcc, 0x52000u) & (1u << 12))) {
        r = giulia_pcie_qos_stop(&wb.qos);
        if (r) rc = r;
    }
    if (wb.gem_clk.state == GIULIA_GEM_ACTIVE || wb.gem_clk.state == GIULIA_GEM_FAULT) {
        r = giulia_gem_clock_stop(&wb.gem_clk);
        if (r && !rc) rc = r;
    }
    if (wb.crm_dev.state != GIULIA_CRM_IDLE && wb.crm_dev.state != GIULIA_CRM_STOPPED) {
        r = giulia_crm_stop(&wb.crm_dev);
        if (r < 0 && !rc) rc = r;
    }
    return rc;
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
    rc = giulia_bcm_pcie0_plan((const uint8_t *)wb.db.address, GIULIA_RPMH_DB_MAX,
        wb.db_version, GIULIA_BCM_PCIE0_FIXED_500_800, &wb.crm_plan);
    wb.crm_rc = rc;
    if (rc) { wb.noc_rc = rc; return rc; }

    wb.noc_step = 2;
    rc = giulia_crm_start_empty_keep(&wb.crm_dev, &crm_io, wb.crm_plan.patch, GIULIA_BCM_PCIE0_PATCHES);
    wb.crm_rc = rc;
    if (rc) { wb.noc_rc = rc; return rc; }

    wb.noc_step = 3;
    if ((rc = giulia_arc_resolve((const uint8_t *)wb.db.address, GIULIA_RPMH_DB_MAX,
            wb.db_version, "xo.lvl", &xo_res)) ||
        (rc = giulia_arc_resolve((const uint8_t *)wb.db.address, GIULIA_RPMH_DB_MAX,
            wb.db_version, "cx.lvl", &cx_res)) ||
        (rc = giulia_arc_select(&cx_res, GIULIA_GEM_CX_AO_MIN_LEVEL, &cx_sel))) {
        wb.arc_rc = rc; wb.noc_rc = rc; return rc;
    }
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
    if (rc) { wb.noc_rc = rc; (void)wb_release_noc(cookie); return rc; }

    wb.noc_step = 0; wb.noc_rc = 0;
    return 0;
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
    wb_map(&wb.dbi, GIULIA_PCIE_LINK_DBI_PA, 0x1000u, VM_WIMG_IO);
    wb_map(&wb.atu, GIULIA_PCIE_LINK_ATU_PA, 0x1000u, VM_WIMG_IO);
    wb_map(&wb.cfg, GIULIA_PCIE_LINK_CFG_PA, GIULIA_PCIE_LINK_CFG_SIZE, VM_WIMG_IO);
    wb_map(&wb.bar0, GIULIA_PCIE_LINK_BAR0_PA, GIULIA_PCIE_LINK_BAR0_SIZE, VM_WIMG_IO);
    wb_map(&wb.crm, GIULIA_NOC_CRM_PHYS, GIULIA_NOC_CRM_SIZE, VM_WIMG_IO);
    wb_map(&wb.qos_pcie, GIULIA_QOS_PCIE_PHYS, GIULIA_QOS_PCIE_SIZE, VM_WIMG_IO);
    wb_map(&wb.qos_gem, GIULIA_QOS_GEM_PHYS, GIULIA_QOS_GEM_SIZE, VM_WIMG_IO);
    if (!wb.gcc.address || !wb.tcsr.address || !wb.tlmm.address || !wb.rsc.address ||
        !wb.db.address || !wb.gic.address || !wb.msg.address || !wb.ipcc.address ||
        !wb.parf.address || !wb.phy.address || !wb.dbi.address || !wb.atu.address ||
        !wb.cfg.address || !wb.bar0.address || !wb.crm.address ||
        !wb.qos_pcie.address || !wb.qos_gem.address) {
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
    if (rc == GIULIA_BRINGUP_OK) {
        if (wb.bringup.stage == GIULIA_BRINGUP_READY) {
            wb_log("ace5-wifi-bringup: soc raw=%#x fam=%u dev=%#x maj=%u min=%u fw=%s mhi_valid=%#x\n",
                wb.bringup.soc.raw, wb.bringup.soc.family, wb.bringup.soc.device,
                wb.bringup.soc.major, wb.bringup.soc.minor,
                giulia_kiwi_fw_name(&wb.bringup.soc), wb.bringup.mhi.valid);
        }
        stop_rc = giulia_wifi_bringup_stop(&wb.bringup);
        wb_log("ace5-wifi-bringup: stop rc=%d stage=%u\n", stop_rc, wb.bringup.stage);
    }

release:
    if (wb_irq_release() && quiet && wb_quiet_req(0))
        wb_log("ace5-wifi-bringup: cleanup complete; firmware resumed (cold reboot required)\n");
done:
    wb_unmap(&wb.qos_gem); wb_unmap(&wb.qos_pcie); wb_unmap(&wb.crm);
    wb_unmap(&wb.bar0); wb_unmap(&wb.cfg); wb_unmap(&wb.atu); wb_unmap(&wb.dbi);
    wb_unmap(&wb.phy); wb_unmap(&wb.parf); wb_unmap(&wb.ipcc); wb_unmap(&wb.msg);
    wb_unmap(&wb.gic); wb_unmap(&wb.db); wb_unmap(&wb.rsc); wb_unmap(&wb.tlmm);
    wb_unmap(&wb.tcsr); wb_unmap(&wb.gcc); wb_unmap(&wb.shared);
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
    if (cpus != 1 || cpu != 1 || ml_get_topology_info()->num_cpus != 1 || seconds < 10 || seconds > 120) {
        wb_log("ace5-wifi-bringup: requires cpus=1 bootcpu=1 and finite 10..120s run\n");
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
