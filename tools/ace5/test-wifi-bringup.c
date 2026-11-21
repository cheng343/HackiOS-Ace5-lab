/* SPDX-License-Identifier: BSD-2-Clause */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../xnu/files/bsd/dev/giulia_wifi_bringup.c"

/* Stubs for lower-level cores so we verify giulia_wifi_bringup ordering,
 * pipe callback binding, SOC info / MHI handoff and reverse teardown. */
static char trace[256];
static unsigned tlen;
static int fail_at = -1;
static char stop_fail, lose_bar;
static int noc_release_rc;
static unsigned host_on, noc_owned, stable_pbl, file_calls, file_present;
static const uint8_t cpu_fixture[]={0x5c,0x72,0x4a};
static const uint8_t *cpu_lookup(const char *dir,const char *name,uint64_t *bytes)
{
    assert(!strcmp(dir,"kiwifw") && !strcmp(name,"amss20.bin"));
    assert(strchr(trace,'H')!=NULL);
    file_calls++;
    *bytes=file_present?sizeof(cpu_fixture):0;
    return file_present?cpu_fixture:NULL;
}
static int ev(char c, int rc) { assert(tlen < sizeof(trace)-1); trace[tlen++]=c; return (int)tlen==fail_at || c==stop_fail ? rc : 0; }

int giulia_wifi_supply_prepare(struct giulia_wifi_supply *p, const struct giulia_wifi_supply_config *c)
{
    int rc = ev('S', GIULIA_SUPPLY_DRIVER);
    p->config = *c; p->state = rc ? GIULIA_SUPPLY_FAULT : GIULIA_SUPPLY_HOST_PREPARED;
    p->gpio.state = GIULIA_WGPIO_ON; p->power.state = GIULIA_POWER_HOST_PREPARED; host_on=1;
    return rc;
}
int giulia_wifi_supply_cancel(struct giulia_wifi_supply *p)
{
    assert(!host_on && !noc_owned);
    int rc=ev('s',GIULIA_SUPPLY_DRIVER); if (rc) return rc;
    p->state = GIULIA_SUPPLY_HELD;
    p->retained = GIULIA_SUPPLY_RETAINS_CPU_NOC | GIULIA_SUPPLY_RETAINS_RAILS;
    return GIULIA_SUPPLY_RETAINED;
}
int giulia_pcie_power_finish_reset(struct giulia_pcie_power *p, uint32_t f)
{ assert(f & GIULIA_POWER_NOC_READY); int rc = ev('P', GIULIA_POWER_TIMEOUT); p->reset_attempted=1; p->state = rc ? GIULIA_POWER_FAULT : GIULIA_POWER_READY; return rc; }
int giulia_pcie_power_pipe_enable(struct giulia_pcie_power *p) { p->state = GIULIA_POWER_PIPE_ON; return ev('E', 0); }
int giulia_pcie_power_pipe_ready(struct giulia_pcie_power *p, uint32_t *observed)
{
    assert(p->state == GIULIA_POWER_PIPE_ON);
    int rc = ev('C', GIULIA_POWER_TIMEOUT);
    *observed = rc ? 0x80000000u : 0x10c00u;
    return rc;
}
int giulia_pcie_power_pipe_disable(struct giulia_pcie_power *p) { p->state = GIULIA_POWER_READY; return ev('e', 0); }
int giulia_pcie_power_stop(struct giulia_pcie_power *p, uint32_t f)
{ assert(f & GIULIA_POWER_STOP_CESTA_RESOURCES_QUIET); assert(host_on && !noc_owned); int rc=ev('p',GIULIA_POWER_TIMEOUT); p->state=rc?GIULIA_POWER_FAULT:GIULIA_POWER_STOPPED; if(!rc)host_on=0; return rc; }
int giulia_pcie_link_prepare_parf(struct giulia_pcie_link *l, uint32_t f, const struct giulia_pcie_link_io *io)
{ assert(f == (GIULIA_LINK_PARF_REQUIRED | GIULIA_LINK_COLD_RESET_MASK)); l->io = *io; int rc = ev('A', GIULIA_LINK_BUSY); l->state = rc ? GIULIA_LINK_FAULT : GIULIA_LINK_PARF_READY; return rc; }
int giulia_pcie_link_prepare(struct giulia_pcie_link *l, uint32_t f)
{ assert(f == GIULIA_LINK_PREPARE_REQUIRED && l->state == GIULIA_LINK_PARF_READY); int rc = ev('L', GIULIA_LINK_BUSY); l->state = rc ? GIULIA_LINK_FAULT : GIULIA_LINK_PREPARED; return rc; }
int giulia_pcie_phy_start(struct giulia_pcie_phy *p, uint32_t f, const struct giulia_pcie_phy_io *io)
{ assert(!!(f & GIULIA_PHY_PCS_BEFORE_PIPE) == !!strchr(trace, 'K')); p->io = *io; int rc = ev('Y', GIULIA_PHY_TIMEOUT); if (!rc) { assert(!io->pipe_enable(io->cookie)); p->state = GIULIA_PHY_READY; } else p->state = GIULIA_PHY_FAULT; return rc; }
int giulia_pcie_phy_stop(struct giulia_pcie_phy *p, uint32_t f)
{ (void)f; int rc=ev('y',GIULIA_PHY_TIMEOUT); if(rc){p->state=GIULIA_PHY_FAULT;return rc;} assert(!p->io.pipe_disable(p->io.cookie)); p->state = GIULIA_PHY_STOPPED; return 0; }
int giulia_wifi_gpio_perst_release(struct giulia_wifi_gpio *g, uint32_t f)
{ (void)f; int rc = ev('R', GIULIA_WGPIO_BUSY); if (!rc) g->state = GIULIA_WGPIO_RELEASED; return rc; }
int giulia_wifi_gpio_perst_assert(struct giulia_wifi_gpio *g, uint32_t f)
{ (void)f; int rc=ev('r',GIULIA_WGPIO_BUSY); if(!rc)g->state = GIULIA_WGPIO_HELD; return rc; }
int giulia_pcie_link_train(struct giulia_pcie_link *l, uint32_t f)
{ (void)f; int rc = ev('T', GIULIA_LINK_TIMEOUT); l->state = rc ? GIULIA_LINK_FAULT : GIULIA_LINK_READY;
  if (!rc) l->endpoint_id=GIULIA_PCIE_LINK_EP_ID;
  return rc; }
int giulia_pcie_link_mem_enable(struct giulia_pcie_link *l, uint32_t f)
{ (void)f; int rc = ev('M', GIULIA_LINK_BAR); if (!rc) l->memory_enabled = 1; return rc; }
int giulia_pcie_link_mem_disable(struct giulia_pcie_link *l, uint32_t f)
{ (void)f; int rc=ev('m',GIULIA_LINK_READBACK); if(!rc)l->memory_enabled = 0; return rc; }
int giulia_pcie_link_quiesce(struct giulia_pcie_link *l, uint32_t f)
{ (void)f; int rc=ev('q',GIULIA_LINK_READBACK); if(!rc)l->state = GIULIA_LINK_QUIESCED; return rc; }
int giulia_pcie_link_stop(struct giulia_pcie_link *l, uint32_t f)
{ (void)f; int rc=ev('l',GIULIA_LINK_READBACK); if(!rc)l->state = GIULIA_LINK_STOPPED; return rc; }
int giulia_kiwi_read_soc_info(const struct giulia_kiwi_bar_io *io, struct giulia_kiwi_soc_info *out)
{ int rc = ev('I', GIULIA_KIWI_READBACK);
  if (lose_bar == 'I') { uint32_t v; assert(io->read32(io->cookie, 0x224, &v) != 0); return GIULIA_KIWI_IO; }
  if (!rc) { out->raw=0x41100201; out->family=4; out->device=0x110; out->major=2; out->minor=1; } return rc; }
const char *giulia_kiwi_fw_name(const struct giulia_kiwi_soc_info *info) { return info->major == 2 ? "amss20.bin" : "amss.bin"; }
int giulia_mhi_snapshot(const struct giulia_mhi_snapshot_io *io, uint32_t f, struct giulia_mhi_snapshot *s)
{ (void)f; int rc = ev('H', GIULIA_MHI_SNAPSHOT_READ_ERROR);
  if (lose_bar == 'H') { uint32_t v; assert(io->read32(io->cookie, 0, &v) != 0); return GIULIA_MHI_SNAPSHOT_READ_ERROR; }
  if (!rc) s->valid = stable_pbl ? GIULIA_MHI_SNAPSHOT_HAVE_HEADER | GIULIA_MHI_SNAPSHOT_HAVE_BHI |
      GIULIA_MHI_SNAPSHOT_HAVE_BHIE | GIULIA_MHI_SNAPSHOT_HAVE_AFTER : GIULIA_MHI_SNAPSHOT_HAVE_HEADER;
  return rc; }

static int noc_prep(void *cookie, struct giulia_wifi_supply *s) { (void)cookie; (void)s; assert(host_on); noc_owned=1; return ev('N', -1); }
static int noc_rel(void *cookie) { (void)cookie; assert(host_on && noc_owned); int rc=ev('n',-1); if(!rc)rc=noc_release_rc; if(!rc)noc_owned=0; return rc; }
static int noc_verify(void *cookie) { (void)cookie; assert(host_on && noc_owned); return ev('V', -1); }
static int arm_pcs_first(void) { assert(host_on && noc_owned); return ev('K', -1); }
static uint32_t d_r32(void *c, unsigned b, uint32_t o) { (void)c; (void)b; (void)o; return 0; }
static void d_w32(void *c, unsigned b, uint32_t o, uint32_t v) { (void)c; (void)b; (void)o; (void)v; }
static uint16_t d_r16(void *c, unsigned b, uint32_t o) { (void)c; (void)b; (void)o; return 0; }
static void d_w16(void *c, unsigned b, uint32_t o, uint16_t v) { (void)c; (void)b; (void)o; (void)v; }
static uint32_t p_r32(void *c, uint32_t o) { (void)c; (void)o; return 0; }
static void p_w32(void *c, uint32_t o, uint32_t v) { (void)c; (void)o; (void)v; }
static void d_bar(void *c) { (void)c; }
static void d_del(void *c, uint32_t u) { (void)c; (void)u; }
static int b_r32(void *c, uint32_t o, uint32_t *v) { (void)c; (void)o; *v = 0; return 0; }

static int lost_bar_lease(void *cookie, uint32_t off, uint32_t *v)
{
    struct giulia_wifi_bringup *b = cookie;
    (void)v;
    assert((b->stage == GIULIA_BRINGUP_SOC_INFO && off == 0x224) ||
        (b->stage == GIULIA_BRINGUP_MHI_SNAP && off == 0));
    giulia_wifi_bringup_hold_bar(b);
    return -1;
}

int main(void)
{
    struct giulia_wifi_bringup b={0};
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
    assert(strcmp(trace, "SNPAYECLRTMIH") == 0);
    assert(giulia_wifi_bringup_stop(&b) == GIULIA_BRINGUP_RETAINED);
    assert(b.stage == GIULIA_BRINGUP_HELD);
    assert(strcmp(trace, "SNPAYECLRTMIHmrqyelnps") == 0);
    assert(!host_on && !noc_owned);
    unsigned before=tlen;
    assert(giulia_wifi_bringup_stop(&b)==GIULIA_BRINGUP_RETAINED && tlen==before);
    struct giulia_wifi_bringup saved=b;
    assert(giulia_wifi_bringup_start(&b,&cfg)==GIULIA_BRINGUP_STATE);
    assert(!memcmp(&b,&saved,sizeof(b)) && tlen==before);

    /* A filename is not a file. Only the successful MHI stage can resolve a
     * CPU view; lack of the optional payload leaves ordinary bringup intact. */
    stable_pbl=1; cfg.fw_lookup=cpu_lookup;
    for (unsigned trial=0;trial<3;trial++) {
        memset(&b,0,sizeof(b)); memset(trace,0,sizeof(trace)); tlen=0;
        cfg.max_stage=trial==0?GIULIA_BRINGUP_SOC_INFO:trial==1?GIULIA_BRINGUP_MHI_SNAP:0;
        file_present=trial==1;
        unsigned calls=file_calls;
        assert(giulia_wifi_bringup_start(&b,&cfg)==GIULIA_BRINGUP_OK);
        assert(b.stage==(trial==0?GIULIA_BRINGUP_SOC_INFO:trial==1?
            GIULIA_BRINGUP_MHI_SNAP:GIULIA_BRINGUP_READY));
        assert(file_calls==calls+(trial!=0));
        if (trial==1) {
            assert(b.fw_image.status==GIULIA_IMAGE_READY && b.fw_image.cpu==cpu_fixture &&
                b.fw_image.plan.image_bytes==sizeof(cpu_fixture) && b.fw_image.cpu[2]==0x4a);
        } else {
            assert(!b.fw_image.cpu && !b.fw_image.plan.image_bytes);
            assert(b.fw_image.status==(trial?GIULIA_IMAGE_ABSENT:GIULIA_IMAGE_NOT_CHECKED));
        }
        assert(giulia_wifi_bringup_stop(&b)==GIULIA_BRINGUP_RETAINED);
        assert(!b.fw_image.cpu && !b.fw_image.name && !b.fw_image.plan.image_bytes &&
            b.fw_image.status==GIULIA_IMAGE_NOT_CHECKED && !host_on && !noc_owned);
    }
    stable_pbl=0; cfg.fw_lookup=NULL; cfg.max_stage=0;

    /* Exercise the real fail_stop/stop path and ensure it issues no I/O. */
    cfg.bar.read32 = cfg.mhi.read32 = lost_bar_lease;
    cfg.bar.cookie = cfg.mhi.cookie = &b;
    for (unsigned pass = 0; pass < 2; pass++) {
        memset(&b, 0, sizeof(b)); memset(trace, 0, sizeof(trace)); tlen = 0;
        giulia_wifi_bringup_hold_bar(&b); assert(b.stage == GIULIA_BRINGUP_IDLE);
        lose_bar = pass ? 'H' : 'I';
        int err = pass ? GIULIA_BRINGUP_MHI : GIULIA_BRINGUP_BAR;
        assert(giulia_wifi_bringup_start(&b, &cfg) == err && b.error == err);
        assert(b.failed_stage == (pass ? GIULIA_BRINGUP_MHI_SNAP : GIULIA_BRINGUP_SOC_INFO));
        assert(b.sub_error == (pass ? GIULIA_MHI_SNAPSHOT_READ_ERROR : GIULIA_KIWI_IO));
        assert(b.stage == GIULIA_BRINGUP_HELD && b.link.memory_enabled && host_on && noc_owned);
        assert(!strcmp(trace, pass ? "SNPAYECLRTMIH" : "SNPAYECLRTMI"));
        before = tlen;
        assert(giulia_wifi_bringup_stop(&b) == GIULIA_BRINGUP_RETAINED && tlen == before);
        assert(giulia_wifi_bringup_start(&b, &cfg) == GIULIA_BRINGUP_STATE && tlen == before);
        host_on = noc_owned = 0; /* simulate a separate cold fixture */
    }
    lose_bar = 0;
    cfg.bar.cookie = cfg.mhi.cookie = NULL;
    cfg.bar.read32 = cfg.mhi.read32 = b_r32;
    puts("PASS: lost KIWI/MHI BAR lease retains the bus without cleanup transactions");

    /* Failure at link train triggers clean reverse unwind. */
    tlen = 0; memset(trace, 0, sizeof(trace)); memset(&b,0,sizeof(b)); fail_at = 10;
    assert(giulia_wifi_bringup_start(&b, &cfg) == GIULIA_BRINGUP_LINK);
    assert(b.failed_stage == GIULIA_BRINGUP_LINK_TRAIN && b.stage == GIULIA_BRINGUP_HELD);
    assert(strcmp(trace, "SNPAYECLRTrqyelnps") == 0);

    tlen = 0; memset(trace, 0, sizeof(trace)); memset(&b,0,sizeof(b));
    fail_at = -1;
    stop_fail = 'C';
    assert(giulia_wifi_bringup_start(&b, &cfg) == GIULIA_BRINGUP_POWER);
    assert(b.failed_stage == GIULIA_BRINGUP_PHY_START && b.sub_error == GIULIA_POWER_TIMEOUT);
    assert(b.pipe_checked && b.pipe_cbcr == 0x80000000u && b.stage == GIULIA_BRINGUP_HELD);
    assert(strcmp(trace, "SNPAYECrqyelnps") == 0);
    stop_fail = 0;

    tlen = 0; memset(trace, 0, sizeof(trace)); memset(&b,0,sizeof(b));
    cfg.verify_noc = noc_verify;
    stop_fail = 'V';
    assert(giulia_wifi_bringup_start(&b, &cfg) == GIULIA_BRINGUP_PREREQUISITE);
    assert(b.failed_stage == GIULIA_BRINGUP_PHY_START && b.stage == GIULIA_BRINGUP_HELD);
    assert(strcmp(trace, "SNPAYECVrqyelnps") == 0);
    cfg.verify_noc = 0;
    stop_fail = 0;

    /* Each startup failure must unwind its actual prerequisites; inactive
     * hooks must not acquire/stop NoC, and a historical reset is not a user. */
    const int startup_faults[]={1,2,3,4,5,7,8,9,10,11,12,13};
    for(unsigned i=0;i<sizeof(startup_faults)/sizeof(startup_faults[0]);i++) {
        memset(&b,0,sizeof(b)); memset(trace,0,sizeof(trace)); tlen=0; fail_at=startup_faults[i];
        assert(giulia_wifi_bringup_start(&b,&cfg)<0);
        assert(b.stage==GIULIA_BRINGUP_HELD && !b.cleanup_error && !host_on && !noc_owned);
    }
    fail_at=-1;
    for(unsigned ceiling=1;ceiling<=GIULIA_BRINGUP_MHI_SNAP;ceiling++) {
        memset(&b,0,sizeof(b)); memset(trace,0,sizeof(trace)); tlen=0; cfg.max_stage=ceiling;
        int rc=giulia_wifi_bringup_start(&b,&cfg);
        if(rc || b.stage!=ceiling)fprintf(stderr,"ceiling=%u rc=%d actual=%u\n",ceiling,rc,b.stage);
        assert(!rc && b.stage==ceiling);
        if (ceiling == GIULIA_BRINGUP_PHY_START) {
            assert(!strcmp(trace, "SNPAYEC") && b.link.state == GIULIA_LINK_PARF_READY);
            assert(b.pipe_checked && b.pipe_cbcr == 0x10c00u);
        }
        assert(giulia_wifi_bringup_stop(&b)==GIULIA_BRINGUP_RETAINED && !host_on && !noc_owned);
    }
    cfg.max_stage=0;
    /* Retained CRM is not withdrawn CESTA: do not drop host power/supplies. */
    memset(&b,0,sizeof(b)); memset(trace,0,sizeof(trace)); tlen=0; noc_release_rc=GIULIA_BRINGUP_RETAINED;
    assert(!giulia_wifi_bringup_start(&b,&cfg));
    assert(giulia_wifi_bringup_stop(&b)==GIULIA_BRINGUP_RETAINED);
    assert(b.stage==GIULIA_BRINGUP_HELD && b.noc_started && host_on && noc_owned);
    assert(!strchr(trace,'p') && !strchr(trace,'s'));
    before=tlen; assert(giulia_wifi_bringup_stop(&b)==GIULIA_BRINGUP_RETAINED && tlen==before);
    noc_release_rc=0;
    /* Stop at the first restoration error. Retry skips completed layers. */
    const char stop_faults[]="mrqylnps";
    for(unsigned i=0;i<sizeof(stop_faults)-1;i++) {
        memset(&b,0,sizeof(b)); memset(trace,0,sizeof(trace)); tlen=0;
        assert(!giulia_wifi_bringup_start(&b,&cfg)); stop_fail=stop_faults[i];
        assert(giulia_wifi_bringup_stop(&b)<0 && b.stage==GIULIA_BRINGUP_FAULT && b.cleanup_error<0);
        if(stop_fail!='s') assert(host_on);
        saved=b; before=tlen;
        assert(giulia_wifi_bringup_start(&b,&cfg)==GIULIA_BRINGUP_STATE && !memcmp(&b,&saved,sizeof(b)) && tlen==before);
        stop_fail=0;
        assert(giulia_wifi_bringup_stop(&b)==GIULIA_BRINGUP_RETAINED && !b.cleanup_error && !host_on && !noc_owned);
    }

    cfg.arm_pcs_first = arm_pcs_first;
    cfg.max_stage = GIULIA_BRINGUP_PHY_START;
    memset(&b,0,sizeof(b)); memset(trace,0,sizeof(trace)); tlen=0;
    stop_fail = 'K';
    assert(giulia_wifi_bringup_start(&b, &cfg) == GIULIA_BRINGUP_PREREQUISITE);
    assert(!strncmp(trace, "SNPAK", 5) && !strchr(trace, 'Y') && !strchr(trace, 'E'));
    assert(!host_on && !noc_owned);
    memset(&b,0,sizeof(b)); memset(trace,0,sizeof(trace)); tlen=0;
    stop_fail = 0;
    assert(!giulia_wifi_bringup_start(&b, &cfg) && !strcmp(trace, "SNPAKYEC"));
    assert(giulia_wifi_bringup_stop(&b)==GIULIA_BRINGUP_RETAINED);

    puts("Wi-Fi end-to-end bringup coordinator tests passed");
    puts("Wi-Fi teardown: NoC before power, retained CRM dependencies, all startup stages and stop-failure retries passed");
    return 0;
}
