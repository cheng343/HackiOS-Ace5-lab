/* SPDX-License-Identifier: BSD-2-Clause */
#include "giulia_wifi_board.h"

static void refresh_owned(struct giulia_wifi_board *b)
{
    b->owned = 0;
    if (b->gpio.state != GIULIA_WGPIO_IDLE && b->gpio.state != GIULIA_WGPIO_STOPPED)
        b->owned |= GIULIA_WBOARD_OWNS_GPIO;
    if (b->power.state != GIULIA_POWER_IDLE && b->power.state != GIULIA_POWER_STOPPED)
        b->owned |= GIULIA_WBOARD_OWNS_POWER;
    if (b->link.state != GIULIA_LINK_IDLE && b->link.state != GIULIA_LINK_STOPPED)
        b->owned |= GIULIA_WBOARD_OWNS_LINK;
    if (b->phy.state != GIULIA_PHY_IDLE && b->phy.state != GIULIA_PHY_STOPPED)
        b->owned |= GIULIA_WBOARD_OWNS_PHY;
}

static int external_valid(struct giulia_wifi_board *b, uint32_t required)
{
    required |= b->conditions & GIULIA_WBOARD_RCHNG_HANDOFF;
    if ((b->conditions & required) != required) return GIULIA_WBOARD_PREREQUISITE;
    b->external_error = b->io.validate_external(b->io.external_cookie, required);
    return b->external_error ? GIULIA_WBOARD_EXTERNAL : GIULIA_WBOARD_OK;
}

static uint32_t gpio_conditions(struct giulia_wifi_board *b)
{
    uint32_t f = 0;
    if (b->conditions & GIULIA_WBOARD_EXCLUSIVE) f |= GIULIA_WGPIO_EXCLUSIVE;
    if (b->conditions & GIULIA_WBOARD_USERS_QUIESCED)
        f |= GIULIA_WGPIO_LINK_DMA_QUIET | GIULIA_WGPIO_MSI_STOPPED;
    if (b->conditions & GIULIA_WBOARD_RAILS_MODES_AOP_READY)
        f |= GIULIA_WGPIO_RAILS_AOP_READY;
    if (b->phy.state == GIULIA_PHY_READY) f |= GIULIA_WGPIO_PHY_READY;
    if (!(b->owned & (GIULIA_WBOARD_OWNS_PHY | GIULIA_WBOARD_OWNS_POWER)))
        f |= GIULIA_WGPIO_PHY_POWER_STOPPED;
    return f;
}
static uint32_t power_conditions(struct giulia_wifi_board *b)
{
    uint32_t f = 0;
    if (b->conditions & GIULIA_WBOARD_MMIO_ACCESSIBLE) f |= GIULIA_POWER_MMIO_OWNED;
    if (b->conditions & GIULIA_WBOARD_EXCLUSIVE) f |= GIULIA_POWER_PCIE0_EXCLUSIVE;
    if (b->conditions & GIULIA_WBOARD_RAILS_MODES_AOP_READY) f |= GIULIA_POWER_RAILS_READY;
    if (b->conditions & GIULIA_WBOARD_NOC_VOTES_READY) f |= GIULIA_POWER_NOC_READY;
    if (b->conditions & GIULIA_WBOARD_CESTA_RELINQUISHED) f |= GIULIA_POWER_DIRECT_VOTES;
    if (b->conditions & GIULIA_WBOARD_PARENT_CLOCKS_READY)
        f |= GIULIA_POWER_XO_19M2_GPLL0_EVEN_300M;
    if (b->perst_isolated) f |= GIULIA_POWER_ENDPOINT_RESET;
    if (b->conditions & GIULIA_WBOARD_USERS_QUIESCED) f |= GIULIA_POWER_LINK_DMA_QUIET;
    if (b->conditions & GIULIA_WBOARD_RCHNG_HANDOFF) f |= GIULIA_POWER_RCHNG_HANDOFF;
    return f;
}
static uint32_t link_conditions(struct giulia_wifi_board *b)
{
    uint32_t f = 0;
    if (b->conditions & GIULIA_WBOARD_EXCLUSIVE) f |= GIULIA_LINK_EXCLUSIVE;
    if ((b->conditions & (GIULIA_WBOARD_RAILS_MODES_AOP_READY | GIULIA_WBOARD_NOC_VOTES_READY)) ==
        (GIULIA_WBOARD_RAILS_MODES_AOP_READY | GIULIA_WBOARD_NOC_VOTES_READY))
        f |= GIULIA_LINK_RAILS_NOC_READY;
    if (b->host_clocks_ready) f |= GIULIA_LINK_CLOCKS_RC_RESET_READY;
    if (b->perst_isolated) f |= GIULIA_LINK_PERST_ASSERTED;
    else if (b->gpio.state == GIULIA_WGPIO_RELEASED)
        f |= GIULIA_LINK_PERST_RELEASED_SETTLED;
    if (b->phy.state == GIULIA_PHY_READY) f |= GIULIA_LINK_PHY_READY;
    if (b->phy.state == GIULIA_PHY_IDLE || b->phy.state == GIULIA_PHY_STOPPED)
        f |= GIULIA_LINK_PHY_QUIET;
    if (b->conditions & GIULIA_WBOARD_USERS_QUIESCED) f |= GIULIA_LINK_USERS_QUIESCED;
    return f;
}
static uint32_t phy_conditions(struct giulia_wifi_board *b)
{
    uint32_t f = 0;
    if (b->conditions & GIULIA_WBOARD_EXCLUSIVE) f |= GIULIA_PHY_EXCLUSIVE;
    if (b->host_clocks_ready) f |= GIULIA_PHY_POWER_CLOCKS_READY;
    if (b->perst_isolated) f |= GIULIA_PHY_PERST_ASSERTED;
    if (!b->link_may_run && (b->conditions & GIULIA_WBOARD_USERS_QUIESCED))
        f |= GIULIA_PHY_LINK_QUIET;
    if (b->link.state == GIULIA_LINK_PARF_READY) f |= GIULIA_PHY_PARF_PREPARED;
    return f;
}

/* Only MMIO callbacks are supplied by the platform; PIPE ownership is always
 * routed through this board's successful power_start, not an unrelated hook. */
static uint32_t phy_read(void *p, uint32_t o)
{ struct giulia_wifi_board *b = p; return b->io.phy.read32(b->io.phy.cookie, o); }
static void phy_write(void *p, uint32_t o, uint32_t v)
{ struct giulia_wifi_board *b = p; b->io.phy.write32(b->io.phy.cookie, o, v); }
static void phy_barrier(void *p)
{ struct giulia_wifi_board *b = p; b->io.phy.barrier(b->io.phy.cookie); }
static void phy_delay(void *p, uint32_t us)
{ struct giulia_wifi_board *b = p; b->io.phy.delay_us(b->io.phy.cookie, us); }
static int pipe_enable(void *p)
{
    struct giulia_wifi_board *b = p;
    int err = external_valid(b, GIULIA_WBOARD_START_REQUIRED);
    if (err) return err;
    if (!b->host_clocks_ready || !b->perst_isolated || b->link_may_run ||
        b->link.state != GIULIA_LINK_PARF_READY) return GIULIA_WBOARD_CONTRACT;
    return giulia_pcie_power_pipe_enable(&b->power);
}
static int pipe_disable(void *p)
{
    struct giulia_wifi_board *b = p;
    int err = external_valid(b, GIULIA_WBOARD_STOP_REQUIRED);
    if (err) return err;
    if (!b->host_clocks_ready || !b->perst_isolated || b->link_may_run)
        return GIULIA_WBOARD_CONTRACT;
    return giulia_pcie_power_pipe_disable(&b->power);
}

static int cleanup_failed(struct giulia_wifi_board *b, int error, int driver_error)
{
    refresh_owned(b);
    b->cleanup_stage = b->stage;
    b->cleanup_error = error;
    b->cleanup_driver_error = driver_error;
    b->state = GIULIA_WBOARD_FAULT;
    return error;
}
static int cleanup(struct giulia_wifi_board *b)
{
    int err, rc;
    refresh_owned(b);
    b->cleanup_error = b->cleanup_driver_error = 0;
    b->cleanup_stage = GIULIA_WBOARD_STAGE_NONE;
    b->state = GIULIA_WBOARD_STOPPING;
    if (!b->owned) goto done;
    if (!(b->owned & GIULIA_WBOARD_OWNS_GPIO))
        return cleanup_failed(b, GIULIA_WBOARD_CONTRACT, 0);
    b->stage = GIULIA_WBOARD_STAGE_ASSERT_PERST;
    if ((err = external_valid(b, GIULIA_WBOARD_STOP_REQUIRED)))
        return cleanup_failed(b, err, 0);
    rc = giulia_wifi_gpio_perst_assert(&b->gpio, gpio_conditions(b));
    if (rc) return cleanup_failed(b, GIULIA_WBOARD_SUBSYSTEM, rc);
    b->perst_isolated = 1;
    refresh_owned(b);
    if (b->owned & GIULIA_WBOARD_OWNS_LINK) {
        b->stage = GIULIA_WBOARD_STAGE_LINK_STOP;
        if ((err = external_valid(b, GIULIA_WBOARD_STOP_REQUIRED)))
            return cleanup_failed(b, err, 0);
        rc = giulia_pcie_link_quiesce(&b->link, link_conditions(b));
        if (rc) return cleanup_failed(b, GIULIA_WBOARD_SUBSYSTEM, rc);
        if (b->link.state != GIULIA_LINK_QUIESCED)
            return cleanup_failed(b, GIULIA_WBOARD_CONTRACT, 0);
    }
    b->link_may_run = 0;
    refresh_owned(b);
    if (b->owned & GIULIA_WBOARD_OWNS_PHY) {
        b->stage = GIULIA_WBOARD_STAGE_PHY_STOP;
        if ((err = external_valid(b, GIULIA_WBOARD_STOP_REQUIRED)))
            return cleanup_failed(b, err, 0);
        rc = giulia_pcie_phy_stop(&b->phy, phy_conditions(b));
        if (rc) return cleanup_failed(b, GIULIA_WBOARD_SUBSYSTEM, rc);
        if (b->phy.state != GIULIA_PHY_STOPPED)
            return cleanup_failed(b, GIULIA_WBOARD_CONTRACT, 0);
    }
    refresh_owned(b);
    if (b->owned & GIULIA_WBOARD_OWNS_LINK) {
        b->stage = GIULIA_WBOARD_STAGE_LINK_RESTORE;
        if ((err = external_valid(b, GIULIA_WBOARD_STOP_REQUIRED)))
            return cleanup_failed(b, err, 0);
        rc = giulia_pcie_link_stop(&b->link, link_conditions(b));
        if (rc) return cleanup_failed(b, GIULIA_WBOARD_SUBSYSTEM, rc);
        if (b->link.state != GIULIA_LINK_STOPPED)
            return cleanup_failed(b, GIULIA_WBOARD_CONTRACT, 0);
    }
    refresh_owned(b);
    if (b->owned & GIULIA_WBOARD_OWNS_POWER) {
        uint32_t f = 0;
        b->stage = GIULIA_WBOARD_STAGE_POWER_STOP;
        if ((err = external_valid(b, GIULIA_WBOARD_STOP_REQUIRED)))
            return cleanup_failed(b, err, 0);
        if (b->perst_isolated) f |= GIULIA_POWER_STOP_ENDPOINT_RESET;
        if (!b->link_may_run && (b->conditions & GIULIA_WBOARD_USERS_QUIESCED))
            f |= GIULIA_POWER_STOP_LINK_DMA_QUIET;
        if (!(b->owned & GIULIA_WBOARD_OWNS_PHY)) f |= GIULIA_POWER_STOP_PHY_QUIET;
        rc = giulia_pcie_power_stop(&b->power, f);
        if (rc) return cleanup_failed(b, GIULIA_WBOARD_SUBSYSTEM, rc);
        if (b->power.state != GIULIA_POWER_STOPPED)
            return cleanup_failed(b, GIULIA_WBOARD_CONTRACT, 0);
    }
    b->host_clocks_ready = 0;
    refresh_owned(b);
    b->stage = GIULIA_WBOARD_STAGE_GPIO_STOP;
    if ((err = external_valid(b, GIULIA_WBOARD_STOP_REQUIRED)))
        return cleanup_failed(b, err, 0);
    rc = giulia_wifi_gpio_stop(&b->gpio, gpio_conditions(b));
    if (rc) return cleanup_failed(b, GIULIA_WBOARD_SUBSYSTEM, rc);
    if (b->gpio.state != GIULIA_WGPIO_STOPPED)
        return cleanup_failed(b, GIULIA_WBOARD_CONTRACT, 0);
done:
    refresh_owned(b);
    if (b->owned) return cleanup_failed(b, GIULIA_WBOARD_CONTRACT, 0);
    b->host_clocks_ready = b->link_may_run = b->perst_isolated = 0;
    b->state = GIULIA_WBOARD_STOPPED;
    return GIULIA_WBOARD_OK;
}

static int start_failed(struct giulia_wifi_board *b, int error, int driver_error)
{
    b->error = error;
    b->driver_error = driver_error;
    b->failed_stage = b->stage;
    (void)cleanup(b);
    return error;
}
static int valid_io(const struct giulia_wifi_board_io *io)
{
    return io && io->validate_external &&
        io->gpio.read32 && io->gpio.write32 && io->gpio.barrier && io->gpio.delay_us &&
        io->power.read32 && io->power.write32 && io->power.barrier && io->power.delay_us &&
        io->link.read32 && io->link.write32 && io->link.read16 && io->link.write16 &&
        io->link.barrier && io->link.delay_us && io->phy.read32 && io->phy.write32 &&
        io->phy.barrier && io->phy.delay_us;
}

int giulia_wifi_board_start(struct giulia_wifi_board *b,
    const struct giulia_wifi_board_io *io, uint32_t conditions)
{
    struct giulia_wifi_board_io copy;
    struct giulia_pcie_phy_io bound_phy;
    int err, rc;
    if (!b || !valid_io(io)) return GIULIA_WBOARD_ARGUMENT;
    if ((conditions & GIULIA_WBOARD_START_REQUIRED) != GIULIA_WBOARD_START_REQUIRED)
        return GIULIA_WBOARD_PREREQUISITE;
    if (b->state != GIULIA_WBOARD_IDLE && b->state != GIULIA_WBOARD_STOPPED)
        return GIULIA_WBOARD_STATE;
    copy = *io;
    *b = (struct giulia_wifi_board){0};
    b->io = copy;
    b->conditions = conditions;
    b->state = GIULIA_WBOARD_STARTING;
#define CHECK_STAGE(which) do { b->stage = (which); \
    if ((err = external_valid(b, GIULIA_WBOARD_START_REQUIRED))) \
        return start_failed(b, err, 0); } while (0)
#define CHECK_RC() do { refresh_owned(b); if (rc) \
    return start_failed(b, GIULIA_WBOARD_SUBSYSTEM, rc); } while (0)
#define EXPECT(condition) do { if (!(condition)) \
    return start_failed(b, GIULIA_WBOARD_CONTRACT, 0); } while (0)
    CHECK_STAGE(GIULIA_WBOARD_STAGE_GPIO_BEGIN);
    rc = giulia_wifi_gpio_begin(&b->gpio, gpio_conditions(b), &b->io.gpio);
    CHECK_RC();
    EXPECT(b->gpio.state == GIULIA_WGPIO_HELD);
    b->perst_isolated = 1;
    CHECK_STAGE(GIULIA_WBOARD_STAGE_WLAN_ON);
    rc = giulia_wifi_gpio_wlan_on(&b->gpio, gpio_conditions(b));
    CHECK_RC();
    EXPECT(b->gpio.state == GIULIA_WGPIO_ON);
    CHECK_STAGE(GIULIA_WBOARD_STAGE_POWER);
    rc = giulia_pcie_power_start(&b->power, &b->io.power, power_conditions(b));
    CHECK_RC();
    EXPECT(b->power.state == GIULIA_POWER_READY);
    b->host_clocks_ready = 1;
    CHECK_STAGE(GIULIA_WBOARD_STAGE_LINK_PREPARE);
    rc = giulia_pcie_link_prepare_parf(&b->link, link_conditions(b), &b->io.link);
    CHECK_RC();
    EXPECT(b->link.state == GIULIA_LINK_PARF_READY && b->link.parf_owned && !b->link.core_owned);
    CHECK_STAGE(GIULIA_WBOARD_STAGE_PHY);
    bound_phy = (struct giulia_pcie_phy_io){b, phy_read, phy_write, phy_barrier,
        phy_delay, pipe_enable, pipe_disable};
    rc = giulia_pcie_phy_start(&b->phy, phy_conditions(b), &bound_phy);
    CHECK_RC();
    EXPECT(b->phy.state == GIULIA_PHY_READY && b->power.state == GIULIA_POWER_PIPE_ON);
    CHECK_STAGE(GIULIA_WBOARD_STAGE_LINK_PREPARE);
    rc = giulia_pcie_link_prepare(&b->link, link_conditions(b));
    CHECK_RC();
    EXPECT(b->link.state == GIULIA_LINK_PREPARED && b->link.parf_owned && b->link.core_owned);
    CHECK_STAGE(GIULIA_WBOARD_STAGE_PERST_RELEASE);
    b->perst_isolated = 0; /* A failed GPIO write might still release reset. */
    rc = giulia_wifi_gpio_perst_release(&b->gpio, gpio_conditions(b));
    CHECK_RC();
    EXPECT(b->gpio.state == GIULIA_WGPIO_RELEASED);
    CHECK_STAGE(GIULIA_WBOARD_STAGE_LINK_TRAIN);
    b->link_may_run = 1;
    rc = giulia_pcie_link_train(&b->link, link_conditions(b));
    CHECK_RC();
    EXPECT(b->link.state == GIULIA_LINK_READY && !b->link.memory_enabled);
    CHECK_STAGE(GIULIA_WBOARD_STAGE_MEMORY);
    rc = giulia_pcie_link_mem_enable(&b->link, link_conditions(b));
    CHECK_RC();
    EXPECT(b->link.state == GIULIA_LINK_READY && b->link.memory_enabled);
    b->state = GIULIA_WBOARD_CPU_BAR_READY;
#undef CHECK_STAGE
#undef CHECK_RC
#undef EXPECT
    return GIULIA_WBOARD_OK;
}

int giulia_wifi_board_stop(struct giulia_wifi_board *b, uint32_t conditions)
{
    if (!b) return GIULIA_WBOARD_ARGUMENT;
    if (b->state == GIULIA_WBOARD_IDLE || b->state == GIULIA_WBOARD_STOPPED)
        return GIULIA_WBOARD_OK;
    if (b->state != GIULIA_WBOARD_CPU_BAR_READY && b->state != GIULIA_WBOARD_FAULT)
        return GIULIA_WBOARD_STATE;
    uint32_t required = GIULIA_WBOARD_STOP_REQUIRED |
        (b->conditions & GIULIA_WBOARD_RCHNG_HANDOFF);
    if ((conditions & required) != required)
        return GIULIA_WBOARD_PREREQUISITE;
    b->conditions = conditions;
    return cleanup(b);
}
