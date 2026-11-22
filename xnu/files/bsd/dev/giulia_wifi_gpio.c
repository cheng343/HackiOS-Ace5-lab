/* SPDX-License-Identifier: BSD-2-Clause */
/* Register facts: Linux v6.12 drivers/pinctrl/qcom/pinctrl-sm8650.c,
 * mux[5:2], drive[8:6], OE[9], pull[1:0], IO output[1], CLKREQ95 mux=1.
 * https://github.com/torvalds/linux/blob/v6.12/drivers/pinctrl/qcom/pinctrl-sm8650.c
 * OnePlus modules/devicetree 453d8875f815bf9dfad9035982861226a4823a6c:
 * pineapple-pinctrl.dtsi and pineapple-kiwi-cnss.dtsi supply board pin facts.
 * CNSS power.c uses a 1 ms WLAN enable delay and 4-5 ms low reset interval;
 * PCIe PERST# is held >=100 ms before release, then >=1 ms before config.
 * This is a new bounded owner/restore implementation, not Linux pinctrl.
 */
#include "giulia_wifi_gpio.h"
#define CFG_MASK 0x3ffu
#define OE (1u << 9)
#define OUT 2u
/* This path has no eGPIO owner: refuse either presence or ownership bit. */
#define PDC_WAKE_GPIO_MASK ((15u << 2) | OE | (1u << 11) | (1u << 12))
#define PDC_WAKE_LOCAL_IRQ 1u
#define PDC_WAKE_PERMISSION (1u << 6)
#define PDC_WAKE_ROUTE (1u << 7)
#define PDC_WAKE_INTR_CFG 8u
static const unsigned pdc_wake_pins[2] = {18, 19};
static const unsigned pins[5] = {16, 94, 95, 96, 17};
static uint32_t read_pin(const struct giulia_wifi_gpio *g, unsigned pin, unsigned reg)
{ return g->io.read32(g->io.cookie, pin * 0x1000u + reg); }

static int lease_ok(const struct giulia_wifi_gpio *g)
{
    return !g->pdc_wake_lease || !g->pdc_wake_lease(g->pdc_wake_cookie);
}

/* One fresh read per access, guarded before touching the GPIO bank. */
static int pdc_wake_read(const struct giulia_wifi_gpio *g, unsigned pin, unsigned reg,
    uint32_t *out)
{
    if (!lease_ok(g)) return GIULIA_WGPIO_PREREQUISITE;
    *out = read_pin(g, pin, reg);
    return *out == UINT32_MAX ? GIULIA_WGPIO_READBACK : GIULIA_WGPIO_OK;
}

static int pdc_wake_read_route(const struct giulia_wifi_gpio *g, unsigned pin, uint32_t *irq)
{
    uint32_t cfg;
    int rc = pdc_wake_read(g, pin, 0, &cfg);
    if (rc) return rc;
    if (cfg & PDC_WAKE_GPIO_MASK) return GIULIA_WGPIO_BUSY;
    rc = pdc_wake_read(g, pin, PDC_WAKE_INTR_CFG, irq);
    if (rc) return rc;
    if ((*irq & (PDC_WAKE_LOCAL_IRQ | PDC_WAKE_PERMISSION)) != PDC_WAKE_PERMISSION)
        return GIULIA_WGPIO_BUSY;
    return GIULIA_WGPIO_OK;
}

static int pdc_wake_check_routes(const struct giulia_wifi_gpio *g)
{
    for (unsigned i = 0; i < 2; i++) {
        uint32_t irq;
        int rc = pdc_wake_read_route(g, pdc_wake_pins[i], &irq);
        if (rc) return rc;
        if (!(irq & PDC_WAKE_ROUTE)) return GIULIA_WGPIO_READBACK;
    }
    return lease_ok(g) ? GIULIA_WGPIO_OK : GIULIA_WGPIO_PREREQUISITE;
}

/* Ace 5 PKG110, confirmed on GPIO18/19 (same-device route, not a generic
 * interrupt enable). See tycykp/UniXNU-OnePlus-Ace5 @ aaa100d19f6f,
 * hardware/sm8650-wifi-pdc-routing.json; the two pins leave local IRQ off. */
int giulia_wifi_gpio_pdc_wake_enable(struct giulia_wifi_gpio *g, void *cookie,
    int (*lease)(void *))
{
    uint32_t before[2];
    int rc;
    if (!g || !lease) return GIULIA_WGPIO_ARGUMENT;
    if (g->state != GIULIA_WGPIO_HELD || !g->bt_owned || g->pdc_wake_lease)
        return GIULIA_WGPIO_STATE;
    g->pdc_wake_cookie = cookie;
    g->pdc_wake_lease = lease;
    /* Reject either unsuitable pin without writing the first one. */
    for (unsigned i = 0; i < 2; i++) {
        rc = pdc_wake_read_route(g, pdc_wake_pins[i], &before[i]);
        if (rc) goto incomplete;
    }
    for (unsigned i = 0; i < 2; i++) {
        unsigned pin = pdc_wake_pins[i];
        uint32_t irq;
        rc = pdc_wake_read_route(g, pin, &irq);
        if (rc) goto incomplete;
        if ((irq ^ before[i]) & PDC_WAKE_ROUTE) {
            rc = GIULIA_WGPIO_BUSY;
            goto incomplete;
        }
        if (irq & PDC_WAKE_ROUTE) continue;
        if (!lease_ok(g)) {
            rc = GIULIA_WGPIO_PREREQUISITE;
            goto incomplete;
        }
        g->pdc_wake_dirty |= 1u << i; /* Before the first potentially posted store. */
        g->io.write32(g->io.cookie, pin * 0x1000u + PDC_WAKE_INTR_CFG, irq | PDC_WAKE_ROUTE);
        g->io.barrier(g->io.cookie);
        rc = pdc_wake_read_route(g, pin, &irq);
        if (rc) goto incomplete;
        if (!(irq & PDC_WAKE_ROUTE)) {
            rc = GIULIA_WGPIO_READBACK;
            goto incomplete;
        }
    }
    rc = pdc_wake_check_routes(g);
    if (rc) goto incomplete;
    g->pdc_wake_active = 1;
    return GIULIA_WGPIO_OK;

incomplete:
    /* Do not attempt another store on a losing transaction. Dirty bits
     * retain its ownership and let an explicit guarded stop restore it. */
    if (!g->pdc_wake_dirty && rc != GIULIA_WGPIO_PREREQUISITE && lease_ok(g)) {
        g->pdc_wake_lease = 0;
        g->pdc_wake_cookie = 0;
    }
    return rc;
}

int giulia_wifi_gpio_pdc_wake_check(const struct giulia_wifi_gpio *g)
{
    if (!g) return GIULIA_WGPIO_ARGUMENT;
    if (!g->pdc_wake_active || !g->pdc_wake_lease || !g->bt_owned ||
        (g->state != GIULIA_WGPIO_HELD && g->state != GIULIA_WGPIO_ON))
        return GIULIA_WGPIO_STATE;
    return pdc_wake_check_routes(g);
}

/* Both endpoint reset and WLAN-off have already been verified by the
 * existing lower GPIO owner. Only undo a bit that this object attempted. */
static int pdc_wake_restore(struct giulia_wifi_gpio *g)
{
    if (!g->pdc_wake_lease) return GIULIA_WGPIO_OK;
    g->pdc_wake_active = 0;
    for (unsigned i = 0; i < 2; i++) {
        unsigned pin = pdc_wake_pins[i];
        uint32_t irq;
        if (!(g->pdc_wake_dirty & (1u << i))) continue;
        int rc = pdc_wake_read_route(g, pin, &irq);
        if (rc) return rc;
        if (irq & PDC_WAKE_ROUTE) {
            if (!lease_ok(g)) return GIULIA_WGPIO_PREREQUISITE;
            g->io.write32(g->io.cookie, pin * 0x1000u + PDC_WAKE_INTR_CFG, irq & ~PDC_WAKE_ROUTE);
            g->io.barrier(g->io.cookie);
            rc = pdc_wake_read_route(g, pin, &irq);
            if (rc) return rc;
            if (irq & PDC_WAKE_ROUTE) return GIULIA_WGPIO_READBACK;
        }
        g->pdc_wake_dirty &= ~(1u << i);
    }
    return lease_ok(g) ? GIULIA_WGPIO_OK : GIULIA_WGPIO_PREREQUISITE;
}

int giulia_wifi_gpio_snapshot(const struct giulia_wifi_gpio *g,
    struct giulia_wifi_gpio_signals *signals)
{
    struct giulia_wifi_gpio_signals sample;
    const struct { unsigned pin; uint32_t *word; } fields[] = {
        {16, &sample.wlan_en}, {18, &sample.sw_ctrl}, {19, &sample.wlan_sw_ctrl},
        {94, &sample.perst}, {95, &sample.clkreq}, {96, &sample.wake}
    };
    if (!g || !signals) return GIULIA_WGPIO_ARGUMENT;
    if (g->state != GIULIA_WGPIO_HELD && g->state != GIULIA_WGPIO_ON &&
        g->state != GIULIA_WGPIO_RELEASED) return GIULIA_WGPIO_STATE;
    if (!g->io.read32) return GIULIA_WGPIO_ARGUMENT;
    for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        uint32_t value = read_pin(g, fields[i].pin, 4);
        if (value == UINT32_MAX) return GIULIA_WGPIO_READBACK;
        *fields[i].word = value;
    }
    *signals = sample;
    return GIULIA_WGPIO_OK;
}
static int change(struct giulia_wifi_gpio *g, unsigned i, unsigned reg,
    uint32_t mask, uint32_t value)
{
    uint32_t old, got;
    if (!lease_ok(g)) return GIULIA_WGPIO_PREREQUISITE;
    old = read_pin(g, pins[i], reg);
    if (old == UINT32_MAX) return GIULIA_WGPIO_READBACK;
    if (!lease_ok(g)) return GIULIA_WGPIO_PREREQUISITE;
    g->dirty |= 1u << i;
    g->io.write32(g->io.cookie, pins[i] * 0x1000u + reg,
        (old & ~mask) | (value & mask));
    g->io.barrier(g->io.cookie);
    if (!lease_ok(g)) return GIULIA_WGPIO_PREREQUISITE;
    got = read_pin(g, pins[i], reg);
    return got != UINT32_MAX && (got & mask) == (value & mask) ? 0 : GIULIA_WGPIO_READBACK;
}
static int failed(struct giulia_wifi_gpio *g)
{ g->state = GIULIA_WGPIO_FAULT; return g->error = GIULIA_WGPIO_READBACK; }
static int conditions(uint32_t got, uint32_t required)
{ return (got & required) == required; }
static int owned_state(uint32_t state)
{
    return state == GIULIA_WGPIO_HELD || state == GIULIA_WGPIO_ON ||
        state == GIULIA_WGPIO_RELEASED || state == GIULIA_WGPIO_FAULT;
}
static void delay(struct giulia_wifi_gpio *g, uint32_t us)
{ g->io.delay_us(g->io.cookie, us); }

static int begin(struct giulia_wifi_gpio *g, uint32_t flags,
    const struct giulia_wifi_gpio_io *io, unsigned take_bt)
{
    uint32_t cfg[5], data[5];
    unsigned count = take_bt ? 5 : 4;
    if (!g || !io || !io->read32 || !io->write32 || !io->barrier || !io->delay_us)
        return GIULIA_WGPIO_ARGUMENT;
    if (g->state != GIULIA_WGPIO_IDLE && g->state != GIULIA_WGPIO_STOPPED)
        return GIULIA_WGPIO_STATE;
    if (!conditions(flags, GIULIA_WGPIO_BEGIN_REQUIRED)) return GIULIA_WGPIO_PREREQUISITE;
    if (take_bt && !(flags & GIULIA_WGPIO_BT_CONTROL_EXCLUSIVE)) return GIULIA_WGPIO_PREREQUISITE;
    g->io = *io;
    g->pdc_wake_active = g->pdc_wake_dirty = 0;
    g->pdc_wake_cookie = 0;
    g->pdc_wake_lease = 0;
    /* Do not cold-reset an active radio or a shared live Bluetooth PMU. */
    uint32_t bt = read_pin(g, 17, 4);
    if (bt == UINT32_MAX) return GIULIA_WGPIO_READBACK;
    if (bt & 3u) return GIULIA_WGPIO_BUSY;
    for (unsigned i = 0; i < count; ++i) {
        uint32_t irq;
        cfg[i] = read_pin(g, pins[i], 0);
        data[i] = read_pin(g, pins[i], 4);
        irq = read_pin(g, pins[i], 8);
        if (cfg[i] == UINT32_MAX || data[i] == UINT32_MAX || irq == UINT32_MAX)
            return GIULIA_WGPIO_READBACK;
        if ((cfg[i] & (1u << 12)) || (irq & 1u)) return GIULIA_WGPIO_BUSY;
    }
    if (data[0] & 3u) return GIULIA_WGPIO_BUSY;
    for (unsigned i = 0; i < count; ++i) {
        g->saved_cfg[i] = cfg[i]; g->saved_io[i] = data[i];
    }
    g->dirty = 0; g->bt_owned = take_bt; g->error = 0; g->state = GIULIA_WGPIO_FAULT;
    /* Output latch first prevents a high pulse while acquiring GPIO mode. */
    if (change(g, 0, 4, OUT, 0) || change(g, 0, 0, CFG_MASK, OE | 1u) ||
        change(g, 1, 4, OUT, 0) || change(g, 1, 0, CFG_MASK, OE | 1u) ||
        change(g, 2, 0, CFG_MASK, 7u) || change(g, 3, 0, CFG_MASK, 3u))
        return failed(g);
    if (take_bt && (change(g, 4, 4, OUT, 0) || change(g, 4, 0, CFG_MASK, OE | 1u)))
        return failed(g);
    delay(g, 5000);
    g->state = GIULIA_WGPIO_HELD;
    return 0;
}
int giulia_wifi_gpio_begin(struct giulia_wifi_gpio *g, uint32_t flags,
    const struct giulia_wifi_gpio_io *io)
{ return begin(g, flags, io, 0); }
int giulia_wifi_gpio_begin_with_bt_reset(struct giulia_wifi_gpio *g, uint32_t flags,
    const struct giulia_wifi_gpio_io *io)
{ return begin(g, flags, io, 1); }
int giulia_wifi_gpio_wlan_on(struct giulia_wifi_gpio *g, uint32_t flags)
{
    if (!g) return GIULIA_WGPIO_ARGUMENT;
    if (g->state != GIULIA_WGPIO_HELD) return GIULIA_WGPIO_STATE;
    if (!conditions(flags, GIULIA_WGPIO_ON_REQUIRED |
        (g->bt_owned ? GIULIA_WGPIO_BT_CONTROL_EXCLUSIVE : 0))) return GIULIA_WGPIO_PREREQUISITE;
    if (g->pdc_wake_lease) {
        int rc = giulia_wifi_gpio_pdc_wake_check(g);
        if (rc) return rc;
    }
    /* Board active state: GPIO, 16 mA, pull-up, output high. */
    if (change(g, 0, 0, CFG_MASK, OE | (7u << 6) | 3u) || change(g, 0, 4, OUT, OUT))
        return failed(g);
    delay(g, 1000);
    g->sw_ctrl = read_pin(g, 18, 4);
    g->wlan_sw_ctrl = read_pin(g, 19, 4);
    if (g->sw_ctrl == UINT32_MAX || g->wlan_sw_ctrl == UINT32_MAX) return failed(g);
    g->state = GIULIA_WGPIO_ON;
    return 0;
}
int giulia_wifi_gpio_perst_release(struct giulia_wifi_gpio *g, uint32_t flags)
{
    if (!g) return GIULIA_WGPIO_ARGUMENT;
    if (g->state != GIULIA_WGPIO_ON) return GIULIA_WGPIO_STATE;
    if (!conditions(flags, GIULIA_WGPIO_RELEASE_REQUIRED |
        (g->bt_owned ? GIULIA_WGPIO_BT_CONTROL_EXCLUSIVE : 0))) return GIULIA_WGPIO_PREREQUISITE;
    if ((read_pin(g, 94, 0) & CFG_MASK) != (OE | 1u) ||
        (read_pin(g, 94, 4) & OUT)) return failed(g);
    delay(g, 100000);
    if (change(g, 1, 4, OUT, OUT)) return failed(g);
    delay(g, 1000);
    g->state = GIULIA_WGPIO_RELEASED;
    return 0;
}
int giulia_wifi_gpio_perst_assert(struct giulia_wifi_gpio *g, uint32_t flags)
{
    if (!g) return GIULIA_WGPIO_ARGUMENT;
    if (!owned_state(g->state)) return GIULIA_WGPIO_STATE;
    if (!conditions(flags, GIULIA_WGPIO_BEGIN_REQUIRED |
        (g->bt_owned ? GIULIA_WGPIO_BT_CONTROL_EXCLUSIVE : 0))) return GIULIA_WGPIO_PREREQUISITE;
    if (change(g, 1, 4, OUT, 0) || change(g, 1, 0, CFG_MASK, OE | 1u)) return failed(g);
    delay(g, 1000);
    if (g->state == GIULIA_WGPIO_RELEASED) g->state = GIULIA_WGPIO_ON;
    return 0;
}
int giulia_wifi_gpio_wlan_off_hold(struct giulia_wifi_gpio *g, uint32_t flags)
{
    if (!g) return GIULIA_WGPIO_ARGUMENT;
    if (!owned_state(g->state)) return GIULIA_WGPIO_STATE;
    if (!conditions(flags, GIULIA_WGPIO_STOP_REQUIRED)) return GIULIA_WGPIO_PREREQUISITE;
    if (g->bt_owned && !(flags & GIULIA_WGPIO_BT_CONTROL_EXCLUSIVE)) return GIULIA_WGPIO_PREREQUISITE;
    if (giulia_wifi_gpio_perst_assert(g, flags) ||
        change(g, 0, 4, OUT, 0) || change(g, 0, 0, CFG_MASK, OE | 1u)) return failed(g);
    /* begin may have failed before acquiring the BT output fields. The
     * exclusive lease and saved snapshot still belong to this object. */
    if (g->bt_owned && (change(g, 4, 4, OUT, 0) || change(g, 4, 0, CFG_MASK, OE | 1u))) return failed(g);
    delay(g, 5000);
    g->state = GIULIA_WGPIO_HELD;
    return 0;
}
int giulia_wifi_gpio_check_held(struct giulia_wifi_gpio *g, uint32_t flags)
{
    static const unsigned check[] = {16,17,94,18,19};
    if (!g) return GIULIA_WGPIO_ARGUMENT;
    if (g->state != GIULIA_WGPIO_HELD || !g->bt_owned) return GIULIA_WGPIO_STATE;
    if (!conditions(flags, GIULIA_WGPIO_BEGIN_REQUIRED | GIULIA_WGPIO_BT_CONTROL_EXCLUSIVE))
        return GIULIA_WGPIO_PREREQUISITE;
    for (unsigned i = 0; i < 5; i++) {
        uint32_t cfg = read_pin(g,check[i],0), data = read_pin(g,check[i],4);
        uint32_t irq = read_pin(g,check[i],8);
        if (cfg == UINT32_MAX || data == UINT32_MAX || irq == UINT32_MAX) return GIULIA_WGPIO_READBACK;
        uint32_t mask = (15u << 2) | OE | (1u << 12), expected = i < 3 ? OE : 0;
        if ((cfg & mask) != expected || (data & 3u) || (irq & 1u)) return GIULIA_WGPIO_BUSY;
    }
    return 0;
}
int giulia_wifi_gpio_stop(struct giulia_wifi_gpio *g, uint32_t flags)
{
    if (!g) return GIULIA_WGPIO_ARGUMENT;
    if (g->state == GIULIA_WGPIO_IDLE || g->state == GIULIA_WGPIO_STOPPED) return 0;
    if (!lease_ok(g)) return GIULIA_WGPIO_PREREQUISITE;
    int rc = giulia_wifi_gpio_wlan_off_hold(g, flags);
    if (rc) return rc;
    rc = pdc_wake_restore(g);
    if (rc) return g->error = rc;
    for (unsigned n = g->bt_owned ? 5 : 4; n > 0; --n) {
        unsigned i = n - 1;
        if (!(g->dirty & (1u << i))) continue;
        /* Release output drive before restoring a previous latch and mux. */
        if (change(g, i, 0, OE, 0) || change(g, i, 4, OUT, g->saved_io[i]) ||
            change(g, i, 0, CFG_MASK, g->saved_cfg[i])) return failed(g);
    }
    g->dirty = 0; g->bt_owned = 0; g->state = GIULIA_WGPIO_STOPPED;
    g->pdc_wake_cookie = 0;
    g->pdc_wake_lease = 0;
    return 0;
}
