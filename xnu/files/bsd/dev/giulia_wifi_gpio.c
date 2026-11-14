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
static const unsigned pins[4] = {16, 94, 95, 96};
static uint32_t read_pin(struct giulia_wifi_gpio *g, unsigned pin, unsigned reg)
{ return g->io.read32(g->io.cookie, pin * 0x1000u + reg); }
static int change(struct giulia_wifi_gpio *g, unsigned i, unsigned reg,
    uint32_t mask, uint32_t value)
{
    uint32_t old = read_pin(g, pins[i], reg), got;
    if (old == UINT32_MAX) return GIULIA_WGPIO_READBACK;
    g->dirty |= 1u << i;
    g->io.write32(g->io.cookie, pins[i] * 0x1000u + reg,
        (old & ~mask) | (value & mask));
    g->io.barrier(g->io.cookie);
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

int giulia_wifi_gpio_begin(struct giulia_wifi_gpio *g, uint32_t flags,
    const struct giulia_wifi_gpio_io *io)
{
    uint32_t cfg[4], data[4];
    if (!g || !io || !io->read32 || !io->write32 || !io->barrier || !io->delay_us)
        return GIULIA_WGPIO_ARGUMENT;
    if (g->state != GIULIA_WGPIO_IDLE && g->state != GIULIA_WGPIO_STOPPED)
        return GIULIA_WGPIO_STATE;
    if (!conditions(flags, GIULIA_WGPIO_BEGIN_REQUIRED)) return GIULIA_WGPIO_PREREQUISITE;
    g->io = *io;
    /* Do not cold-reset an active radio or a shared live Bluetooth PMU. */
    uint32_t bt = read_pin(g, 17, 4);
    if (bt == UINT32_MAX) return GIULIA_WGPIO_READBACK;
    if (bt & 3u) return GIULIA_WGPIO_BUSY;
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t irq;
        cfg[i] = read_pin(g, pins[i], 0);
        data[i] = read_pin(g, pins[i], 4);
        irq = read_pin(g, pins[i], 8);
        if (cfg[i] == UINT32_MAX || data[i] == UINT32_MAX || irq == UINT32_MAX)
            return GIULIA_WGPIO_READBACK;
        if ((cfg[i] & (1u << 12)) || (irq & 1u)) return GIULIA_WGPIO_BUSY;
    }
    if (data[0] & 3u) return GIULIA_WGPIO_BUSY;
    for (unsigned i = 0; i < 4; ++i) {
        g->saved_cfg[i] = cfg[i]; g->saved_io[i] = data[i];
    }
    g->dirty = 0; g->error = 0; g->state = GIULIA_WGPIO_FAULT;
    /* Output latch first prevents a high pulse while acquiring GPIO mode. */
    if (change(g, 0, 4, OUT, 0) || change(g, 0, 0, CFG_MASK, OE | 1u) ||
        change(g, 1, 4, OUT, 0) || change(g, 1, 0, CFG_MASK, OE | 1u) ||
        change(g, 2, 0, CFG_MASK, 7u) || change(g, 3, 0, CFG_MASK, 3u))
        return failed(g);
    delay(g, 5000);
    g->state = GIULIA_WGPIO_HELD;
    return 0;
}
int giulia_wifi_gpio_wlan_on(struct giulia_wifi_gpio *g, uint32_t flags)
{
    if (!g) return GIULIA_WGPIO_ARGUMENT;
    if (g->state != GIULIA_WGPIO_HELD) return GIULIA_WGPIO_STATE;
    if (!conditions(flags, GIULIA_WGPIO_ON_REQUIRED)) return GIULIA_WGPIO_PREREQUISITE;
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
    if (!conditions(flags, GIULIA_WGPIO_RELEASE_REQUIRED)) return GIULIA_WGPIO_PREREQUISITE;
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
    if (!conditions(flags, GIULIA_WGPIO_BEGIN_REQUIRED)) return GIULIA_WGPIO_PREREQUISITE;
    if (change(g, 1, 4, OUT, 0) || change(g, 1, 0, CFG_MASK, OE | 1u)) return failed(g);
    delay(g, 1000);
    if (g->state == GIULIA_WGPIO_RELEASED) g->state = GIULIA_WGPIO_ON;
    return 0;
}
int giulia_wifi_gpio_stop(struct giulia_wifi_gpio *g, uint32_t flags)
{
    if (!g) return GIULIA_WGPIO_ARGUMENT;
    if (g->state == GIULIA_WGPIO_IDLE || g->state == GIULIA_WGPIO_STOPPED) return 0;
    if (!owned_state(g->state)) return GIULIA_WGPIO_STATE;
    if (!conditions(flags, GIULIA_WGPIO_STOP_REQUIRED)) return GIULIA_WGPIO_PREREQUISITE;
    if (giulia_wifi_gpio_perst_assert(g, flags) ||
        change(g, 0, 4, OUT, 0) || change(g, 0, 0, CFG_MASK, OE | 1u)) return failed(g);
    delay(g, 5000);
    for (unsigned n = 4; n > 0; --n) {
        unsigned i = n - 1;
        if (!(g->dirty & (1u << i))) continue;
        /* Release output drive before restoring a previous latch and mux. */
        if (change(g, i, 0, OE, 0) || change(g, i, 4, OUT, g->saved_io[i]) ||
            change(g, i, 0, CFG_MASK, g->saved_cfg[i])) return failed(g);
    }
    g->dirty = 0; g->state = GIULIA_WGPIO_STOPPED;
    return 0;
}
