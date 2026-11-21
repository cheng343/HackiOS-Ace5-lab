/* SPDX-License-Identifier: BSD-2-Clause */
#include "giulia_wifi_bringup.h"

static int pipe_on(void *cookie)
{
    struct giulia_wifi_bringup *b = cookie;
    return giulia_pcie_power_pipe_enable(&b->supply.power);
}

static int pipe_off(void *cookie)
{
    struct giulia_wifi_bringup *b = cookie;
    return giulia_pcie_power_pipe_disable(&b->supply.power);
}

static uint32_t power_finish_flags(const struct giulia_wifi_bringup *b)
{
    uint32_t f = GIULIA_POWER_HOST_REQUIRED | GIULIA_POWER_NOC_READY |
        GIULIA_POWER_CESTA_COORDINATED;
    if (b->cfg.supply.conditions & GIULIA_SUPPLY_RCHNG_HANDOFF)
        f |= GIULIA_POWER_RCHNG_HANDOFF;
    return f;
}

static int fail_stop(struct giulia_wifi_bringup *b, uint32_t stage, int err, int sub)
{
    int ce;
    b->failed_stage = stage;
    b->error = err;
    b->sub_error = sub;
    ce = giulia_wifi_bringup_stop(b);
    if (ce < 0) b->cleanup_error = ce;
    return err;
}

int giulia_wifi_bringup_start(struct giulia_wifi_bringup *b,
    const struct giulia_wifi_bringup_config *cfg)
{
    int rc;
    struct giulia_pcie_phy_io phy_io;
    uint32_t link_base = GIULIA_LINK_EXCLUSIVE | GIULIA_LINK_RAILS_NOC_READY |
        GIULIA_LINK_CLOCKS_RC_RESET_READY;
    uint32_t phy_req = GIULIA_PHY_START_REQUIRED;

    if (!b || !cfg || !cfg->bar.read32 || !cfg->mhi.read32 ||
        !cfg->phy.read32 || !cfg->phy.write32 || !cfg->phy.barrier || !cfg->phy.delay_us ||
        !cfg->link.read32 || !cfg->link.write32 || !cfg->link.read16 ||
        !cfg->link.write16 || !cfg->link.barrier || !cfg->link.delay_us)
        return GIULIA_BRINGUP_ARGUMENT;

    if (b->stage != GIULIA_BRINGUP_IDLE && b->stage != GIULIA_BRINGUP_STOPPED)
        return GIULIA_BRINGUP_STATE;
    if (!!cfg->prepare_noc != !!cfg->release_noc || cfg->max_stage > GIULIA_BRINGUP_MHI_SNAP)
        return GIULIA_BRINGUP_ARGUMENT;
    if (cfg->arm_pcs_first && cfg->max_stage != GIULIA_BRINGUP_PHY_START)
        return GIULIA_BRINGUP_PREREQUISITE;
    struct giulia_wifi_bringup_config copy = *cfg;
    *b = (struct giulia_wifi_bringup){0};
    b->cfg = copy;

    b->stage = GIULIA_BRINGUP_SUPPLY_STAGE;
    rc = giulia_wifi_supply_prepare(&b->supply, &b->cfg.supply);
    if (rc) return fail_stop(b, GIULIA_BRINGUP_SUPPLY_STAGE, GIULIA_BRINGUP_SUPPLY, rc);
    if (b->cfg.max_stage && b->stage >= b->cfg.max_stage) return GIULIA_BRINGUP_OK;

    if (b->cfg.prepare_noc) {
        b->stage = GIULIA_BRINGUP_NOC_STAGE;
        b->noc_started = 1; /* A failed callback may have published requests. */
        rc = b->cfg.prepare_noc(b->cfg.noc_cookie, &b->supply);
        if (rc) return fail_stop(b, GIULIA_BRINGUP_NOC_STAGE, GIULIA_BRINGUP_PREREQUISITE, rc);
        if (b->cfg.max_stage && b->stage >= b->cfg.max_stage) return GIULIA_BRINGUP_OK;
    }

    b->stage = GIULIA_BRINGUP_POWER_RESET;
    rc = giulia_pcie_power_finish_reset(&b->supply.power, power_finish_flags(b));
    if (rc) return fail_stop(b, GIULIA_BRINGUP_POWER_RESET, GIULIA_BRINGUP_POWER, rc);
    if (b->cfg.max_stage && b->stage >= b->cfg.max_stage) return GIULIA_BRINGUP_OK;

    b->stage = GIULIA_BRINGUP_PHY_START;
    /* SM8650 resets PARF_INT_MASK (0x22c) non-zero after GCC_PCIE_0_BCR. */
    rc = giulia_pcie_link_prepare_parf(&b->link,
        link_base | GIULIA_LINK_PERST_ASSERTED | GIULIA_LINK_COLD_RESET_MASK,
        &b->cfg.link);
    if (rc) return fail_stop(b, GIULIA_BRINGUP_PHY_START, GIULIA_BRINGUP_LINK, rc);
    phy_io = b->cfg.phy;
    phy_io.cookie = b;
    phy_io.pipe_enable = pipe_on;
    phy_io.pipe_disable = pipe_off;
    if (b->cfg.arm_pcs_first) {
        rc = b->cfg.arm_pcs_first();
        if (rc) return fail_stop(b, GIULIA_BRINGUP_PHY_START, GIULIA_BRINGUP_PREREQUISITE, rc);
        phy_req |= GIULIA_PHY_PCS_BEFORE_PIPE;
    }
    rc = giulia_pcie_phy_start(&b->phy, phy_req, &phy_io);
    if (rc) return fail_stop(b, GIULIA_BRINGUP_PHY_START, GIULIA_BRINGUP_PHY, rc);
    b->pipe_checked = 1;
    rc = giulia_pcie_power_pipe_ready(&b->supply.power, &b->pipe_cbcr);
    if (rc) return fail_stop(b, GIULIA_BRINGUP_PHY_START, GIULIA_BRINGUP_POWER, rc);
    if (b->cfg.verify_noc) {
        rc = b->cfg.verify_noc(b->cfg.noc_cookie);
        if (rc) return fail_stop(b, GIULIA_BRINGUP_PHY_START, GIULIA_BRINGUP_PREREQUISITE, rc);
    }
    if (b->cfg.max_stage && b->stage >= b->cfg.max_stage) return GIULIA_BRINGUP_OK;

    b->stage = GIULIA_BRINGUP_LINK_PREPARE;
    rc = giulia_pcie_link_prepare(&b->link, link_base | GIULIA_LINK_PERST_ASSERTED |
        GIULIA_LINK_PHY_READY);
    if (rc) return fail_stop(b, GIULIA_BRINGUP_LINK_PREPARE, GIULIA_BRINGUP_LINK, rc);
    if (b->cfg.max_stage && b->stage >= b->cfg.max_stage) return GIULIA_BRINGUP_OK;

    b->stage = GIULIA_BRINGUP_PERST_RELEASE;
    rc = giulia_wifi_gpio_perst_release(&b->supply.gpio,
        GIULIA_WGPIO_RELEASE_REQUIRED | GIULIA_WGPIO_BT_CONTROL_EXCLUSIVE);
    if (rc) return fail_stop(b, GIULIA_BRINGUP_PERST_RELEASE, GIULIA_BRINGUP_GPIO, rc);
    if (b->cfg.max_stage && b->stage >= b->cfg.max_stage) return GIULIA_BRINGUP_OK;

    b->stage = GIULIA_BRINGUP_LINK_TRAIN;
    rc = giulia_pcie_link_train(&b->link,
        link_base | GIULIA_LINK_PHY_READY | GIULIA_LINK_PERST_RELEASED_SETTLED);
    if (rc) return fail_stop(b, GIULIA_BRINGUP_LINK_TRAIN, GIULIA_BRINGUP_LINK, rc);
    if (b->cfg.max_stage && b->stage >= b->cfg.max_stage) return GIULIA_BRINGUP_OK;

    b->stage = GIULIA_BRINGUP_MEM_ENABLE;
    rc = giulia_pcie_link_mem_enable(&b->link,
        link_base | GIULIA_LINK_PHY_READY | GIULIA_LINK_PERST_RELEASED_SETTLED |
        GIULIA_LINK_USERS_QUIESCED);
    if (rc) return fail_stop(b, GIULIA_BRINGUP_MEM_ENABLE, GIULIA_BRINGUP_LINK, rc);
    if (b->cfg.max_stage && b->stage >= b->cfg.max_stage) return GIULIA_BRINGUP_OK;

    b->stage = GIULIA_BRINGUP_SOC_INFO;
    rc = giulia_kiwi_read_soc_info(&b->cfg.bar, &b->soc);
    if (rc) return fail_stop(b, GIULIA_BRINGUP_SOC_INFO, GIULIA_BRINGUP_BAR, rc);
    if (b->cfg.max_stage && b->stage >= b->cfg.max_stage) return GIULIA_BRINGUP_OK;

    b->stage = GIULIA_BRINGUP_MHI_SNAP;
    rc = giulia_mhi_snapshot(&b->cfg.mhi, GIULIA_MHI_SNAPSHOT_REQUIRED, &b->mhi);
    if (rc) return fail_stop(b, GIULIA_BRINGUP_MHI_SNAP, GIULIA_BRINGUP_MHI, rc);
    /* Optional local file only; never turn a missing/ineligible firmware
     * into a failed hardware snapshot, or a sampled READY into a booted EE. */
    giulia_kiwi_image_resolve(&b->fw_image,b->link.endpoint_id,&b->soc,&b->mhi,b->cfg.fw_lookup);
    if (b->cfg.max_stage && b->stage >= b->cfg.max_stage) return GIULIA_BRINGUP_OK;

    b->stage = GIULIA_BRINGUP_READY;
    return GIULIA_BRINGUP_OK;
}

int giulia_wifi_bringup_stop(struct giulia_wifi_bringup *b)
{
    int rc;
    uint32_t link_base = GIULIA_LINK_EXCLUSIVE | GIULIA_LINK_RAILS_NOC_READY |
        GIULIA_LINK_CLOCKS_RC_RESET_READY;
    if (!b) return GIULIA_BRINGUP_ARGUMENT;
    b->fw_image=(struct giulia_kiwi_image){0}; /* retire the borrowed view even on retained stop */
    if (b->stage == GIULIA_BRINGUP_IDLE || b->stage == GIULIA_BRINGUP_STOPPED)
        return GIULIA_BRINGUP_OK;
    if (b->stage == GIULIA_BRINGUP_HELD) return GIULIA_BRINGUP_RETAINED;
    b->stage = GIULIA_BRINGUP_STOPPING;
    b->cleanup_error = 0;
#define STOP_FAILED(e) do { b->cleanup_error = (e); b->stage = GIULIA_BRINGUP_FAULT; return (e); } while (0)

    if (b->link.memory_enabled) {
        rc = giulia_pcie_link_mem_disable(&b->link,
            link_base | GIULIA_LINK_PHY_READY | GIULIA_LINK_PERST_RELEASED_SETTLED |
            GIULIA_LINK_USERS_QUIESCED);
        if (rc) STOP_FAILED(GIULIA_BRINGUP_LINK);
    }
    if (b->supply.gpio.state == GIULIA_WGPIO_ON ||
        b->supply.gpio.state == GIULIA_WGPIO_RELEASED) {
        rc = giulia_wifi_gpio_perst_assert(&b->supply.gpio,
            GIULIA_WGPIO_BEGIN_REQUIRED | GIULIA_WGPIO_BT_CONTROL_EXCLUSIVE);
        if (rc) STOP_FAILED(GIULIA_BRINGUP_GPIO);
    }
    if (b->link.state != GIULIA_LINK_IDLE && b->link.state != GIULIA_LINK_STOPPED) {
        uint32_t flags = GIULIA_LINK_QUIESCE_REQUIRED;
        if (b->phy.state == GIULIA_PHY_READY) flags |= GIULIA_LINK_PHY_READY;
        rc = giulia_pcie_link_quiesce(&b->link, flags);
        if (rc) STOP_FAILED(GIULIA_BRINGUP_LINK);
    }
    if (b->phy.state != GIULIA_PHY_IDLE && b->phy.state != GIULIA_PHY_STOPPED) {
        rc = giulia_pcie_phy_stop(&b->phy, GIULIA_PHY_STOP_REQUIRED);
        if (rc) STOP_FAILED(GIULIA_BRINGUP_PHY);
    }
    if (b->link.state == GIULIA_LINK_QUIESCED) {
        rc = giulia_pcie_link_stop(&b->link, GIULIA_LINK_STOP_REQUIRED);
        if (rc) STOP_FAILED(GIULIA_BRINGUP_LINK);
    }
    /* QoS needs the PCIe/GEM configuration clocks. In particular, power_stop
     * restores GCC vote12: doing that first can strand the QoS owner. Never
     * claim CESTA_RESOURCES_QUIET when the callback retained a CRM channel. */
    if (b->noc_started) {
        rc = b->cfg.release_noc(b->cfg.noc_cookie);
        if (rc == GIULIA_BRINGUP_RETAINED) {
            b->stage = GIULIA_BRINGUP_HELD;
            return rc;
        }
        if (rc) STOP_FAILED(GIULIA_BRINGUP_PREREQUISITE);
        b->noc_started = 0;
    }
    if (b->supply.power.state != GIULIA_POWER_IDLE &&
        b->supply.power.state != GIULIA_POWER_STOPPED) {
        rc = giulia_pcie_power_stop(&b->supply.power,
            GIULIA_POWER_STOP_REQUIRED | GIULIA_POWER_STOP_CESTA_RESOURCES_QUIET);
        if (rc) STOP_FAILED(GIULIA_BRINGUP_POWER);
    }
    rc = giulia_wifi_supply_cancel(&b->supply);
    if (rc == GIULIA_SUPPLY_RETAINED) {
        b->stage = GIULIA_BRINGUP_HELD;
        return GIULIA_BRINGUP_RETAINED;
    }
    if (rc) STOP_FAILED(GIULIA_BRINGUP_SUPPLY);
    b->stage = GIULIA_BRINGUP_STOPPED;
    return GIULIA_BRINGUP_OK;
#undef STOP_FAILED
}
