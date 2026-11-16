/* SPDX-License-Identifier: BSD-2-Clause */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../xnu/files/bsd/dev/giulia_wifi_bringup.c"

/* Stubs for lower-level cores so we verify giulia_wifi_bringup ordering,
 * pipe callback binding, SOC info / MHI handoff and reverse teardown. */
static char trace[64];
static unsigned tlen;
static int fail_at = -1;
static int ev(char c, int rc) { assert(tlen < sizeof(trace)-1); trace[tlen++]=c; return (int)tlen==fail_at ? rc : 0; }

int giulia_wifi_supply_prepare(struct giulia_wifi_supply *p, const struct giulia_wifi_supply_config *c)
{
    int rc = ev('S', GIULIA_SUPPLY_DRIVER);
    p->config = *c; p->state = rc ? GIULIA_SUPPLY_FAULT : GIULIA_SUPPLY_HOST_PREPARED;
    p->gpio.state = GIULIA_WGPIO_ON; p->power.state = GIULIA_POWER_HOST_PREPARED;
    return rc;
}
int giulia_wifi_supply_cancel(struct giulia_wifi_supply *p)
{
    ev('s', 0); p->state = GIULIA_SUPPLY_HELD;
    p->retained = GIULIA_SUPPLY_RETAINS_CPU_NOC | GIULIA_SUPPLY_RETAINS_RAILS;
    return GIULIA_SUPPLY_RETAINED;
}
int giulia_pcie_power_finish_reset(struct giulia_pcie_power *p, uint32_t f)
{ assert(f & GIULIA_POWER_NOC_READY); int rc = ev('P', GIULIA_POWER_TIMEOUT); p->state = rc ? GIULIA_POWER_FAULT : GIULIA_POWER_READY; return rc; }
int giulia_pcie_power_pipe_enable(struct giulia_pcie_power *p) { p->state = GIULIA_POWER_PIPE_ON; return ev('E', 0); }
int giulia_pcie_power_pipe_disable(struct giulia_pcie_power *p) { p->state = GIULIA_POWER_READY; return ev('e', 0); }
int giulia_pcie_power_stop(struct giulia_pcie_power *p, uint32_t f) { (void)f; p->state = GIULIA_POWER_STOPPED; return ev('p', 0); }
int giulia_pcie_link_prepare(struct giulia_pcie_link *l, uint32_t f, const struct giulia_pcie_link_io *io)
{ (void)f; l->io = *io; int rc = ev('L', GIULIA_LINK_BUSY); l->state = rc ? GIULIA_LINK_FAULT : GIULIA_LINK_PREPARED; return rc; }
int giulia_pcie_phy_start(struct giulia_pcie_phy *p, uint32_t f, const struct giulia_pcie_phy_io *io)
{ (void)f; p->io = *io; int rc = ev('Y', GIULIA_PHY_TIMEOUT); if (!rc) { assert(!io->pipe_enable(io->cookie)); p->state = GIULIA_PHY_READY; } else p->state = GIULIA_PHY_FAULT; return rc; }
int giulia_pcie_phy_stop(struct giulia_pcie_phy *p, uint32_t f)
{ (void)f; assert(!p->io.pipe_disable(p->io.cookie)); p->state = GIULIA_PHY_STOPPED; return ev('y', 0); }
int giulia_wifi_gpio_perst_release(struct giulia_wifi_gpio *g, uint32_t f)
{ (void)f; int rc = ev('R', GIULIA_WGPIO_BUSY); if (!rc) g->perst_released = 1; return rc; }
int giulia_wifi_gpio_perst_assert(struct giulia_wifi_gpio *g, uint32_t f)
{ (void)f; g->perst_released = 0; g->state = GIULIA_WGPIO_HELD; return ev('r', 0); }
int giulia_pcie_link_train(struct giulia_pcie_link *l, uint32_t f)
{ (void)f; int rc = ev('T', GIULIA_LINK_TIMEOUT); l->state = rc ? GIULIA_LINK_FAULT : GIULIA_LINK_READY; return rc; }
int giulia_pcie_link_mem_enable(struct giulia_pcie_link *l, uint32_t f)
{ (void)f; int rc = ev('M', GIULIA_LINK_BAR); if (!rc) l->memory_enabled = 1; return rc; }
int giulia_pcie_link_mem_disable(struct giulia_pcie_link *l, uint32_t f)
{ (void)f; l->memory_enabled = 0; return ev('m', 0); }
int giulia_pcie_link_quiesce(struct giulia_pcie_link *l, uint32_t f)
{ (void)f; l->state = GIULIA_LINK_QUIESCED; return ev('q', 0); }
int giulia_pcie_link_stop(struct giulia_pcie_link *l, uint32_t f)
{ (void)f; l->state = GIULIA_LINK_STOPPED; return ev('l', 0); }
int giulia_kiwi_read_soc_info(const struct giulia_kiwi_bar_io *io, struct giulia_kiwi_soc_info *out)
{ (void)io; int rc = ev('I', GIULIA_KIWI_READBACK); if (!rc) { out->family=4; out->device=0x110; out->major=2; out->minor=1; } return rc; }
const char *giulia_kiwi_fw_name(const struct giulia_kiwi_soc_info *info) { return info->major == 2 ? "amss20.bin" : "amss.bin"; }
int giulia_mhi_snapshot(const struct giulia_mhi_snapshot_io *io, uint32_t f, struct giulia_mhi_snapshot *s)
{ (void)io; (void)f; int rc = ev('H', GIULIA_MHI_SNAPSHOT_READ_ERROR); if (!rc) s->valid = GIULIA_MHI_SNAPSHOT_HAVE_HEADER; return rc; }

static int noc_prep(void *cookie, struct giulia_wifi_supply *s) { (void)cookie; (void)s; return ev('N', -1); }
static int noc_rel(void *cookie) { (void)cookie; return ev('n', 0); }
static uint32_t d_r32(void *c, unsigned b, uint32_t o) { (void)c; (void)b; (void)o; return 0; }
static void d_w32(void *c, unsigned b, uint32_t o, uint32_t v) { (void)c; (void)b; (void)o; (void)v; }
static uint16_t d_r16(void *c, unsigned b, uint32_t o) { (void)c; (void)b; (void)o; return 0; }
static void d_w16(void *c, unsigned b, uint32_t o, uint16_t v) { (void)c; (void)b; (void)o; (void)v; }
static uint32_t p_r32(void *c, uint32_t o) { (void)c; (void)o; return 0; }
static void p_w32(void *c, uint32_t o, uint32_t v) { (void)c; (void)o; (void)v; }
static void d_bar(void *c) { (void)c; }
static void d_del(void *c, uint32_t u) { (void)c; (void)u; }
static int b_r32(void *c, uint32_t o, uint32_t *v) { (void)c; (void)o; *v = 0; return 0; }

int main(void)
{
    struct giulia_wifi_bringup b;
    struct giulia_wifi_bringup_config cfg = {0};
    cfg.link.read32 = d_r32; cfg.link.write32 = d_w32; cfg.link.read16 = d_r16;
    cfg.link.write16 = d_w16; cfg.link.barrier = d_bar; cfg.link.delay_us = d_del;
    cfg.phy.read32 = p_r32; cfg.phy.write32 = p_w32; cfg.phy.barrier = d_bar; cfg.phy.delay_us = d_del;
    cfg.bar.read32 = b_r32; cfg.bar.bar_bytes = 0x200000u;
    cfg.mhi.read32 = b_r32; cfg.mhi.bar_bytes = 0x200000u;
    cfg.prepare_noc = noc_prep; cfg.release_noc = noc_rel;

    tlen = 0; fail_at = -1;
    assert(giulia_wifi_bringup_start(&b, &cfg) == GIULIA_BRINGUP_OK);
    assert(b.stage == GIULIA_BRINGUP_READY && strcmp(giulia_kiwi_fw_name(&b.soc), "amss20.bin") == 0);
    assert(strcmp(trace, "SNPLYERTMIH") == 0);
    assert(giulia_wifi_bringup_stop(&b) == GIULIA_BRINGUP_RETAINED);
    assert(b.stage == GIULIA_BRINGUP_HELD);
    assert(strcmp(trace, "SNPLYERTMIHmrqeylpns") == 0);

    /* Failure at link train (step 8 = 'T') triggers clean reverse unwind */
    tlen = 0; memset(trace, 0, sizeof(trace)); fail_at = 8;
    assert(giulia_wifi_bringup_start(&b, &cfg) == GIULIA_BRINGUP_LINK);
    assert(b.failed_stage == GIULIA_BRINGUP_LINK_TRAIN && b.stage == GIULIA_BRINGUP_HELD);
    assert(strcmp(trace, "SNPLYERTrqeylpns") == 0);

    puts("Wi-Fi end-to-end bringup coordinator tests passed");
    return 0;
}
