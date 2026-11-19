/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_WIFI_GPIO_H
#define GIULIA_WIFI_GPIO_H
#include <stdint.h>

/* SM8650 TLMM GPIO bank, not the TLMM controller base. */
#define GIULIA_WIFI_GPIO_PA 0x0f100000u
#define GIULIA_WIFI_GPIO_SIZE 0x00100000u
enum giulia_wifi_gpio_result {
    GIULIA_WGPIO_OK = 0, GIULIA_WGPIO_ARGUMENT = -1,
    GIULIA_WGPIO_STATE = -2, GIULIA_WGPIO_PREREQUISITE = -3,
    GIULIA_WGPIO_BUSY = -4, GIULIA_WGPIO_READBACK = -5
};
enum giulia_wifi_gpio_state {
    GIULIA_WGPIO_IDLE, GIULIA_WGPIO_HELD, GIULIA_WGPIO_ON,
    GIULIA_WGPIO_RELEASED, GIULIA_WGPIO_FAULT, GIULIA_WGPIO_STOPPED
};
enum giulia_wifi_gpio_condition {
    GIULIA_WGPIO_EXCLUSIVE = 1u << 0,
    GIULIA_WGPIO_LINK_DMA_QUIET = 1u << 1,
    GIULIA_WGPIO_RAILS_AOP_READY = 1u << 2,
    GIULIA_WGPIO_PHY_READY = 1u << 3,
    GIULIA_WGPIO_MSI_STOPPED = 1u << 4,
    GIULIA_WGPIO_PHY_POWER_STOPPED = 1u << 5,
    GIULIA_WGPIO_BT_CONTROL_EXCLUSIVE = 1u << 6
};
#define GIULIA_WGPIO_BEGIN_REQUIRED (GIULIA_WGPIO_EXCLUSIVE | GIULIA_WGPIO_LINK_DMA_QUIET)
#define GIULIA_WGPIO_ON_REQUIRED (GIULIA_WGPIO_BEGIN_REQUIRED | GIULIA_WGPIO_RAILS_AOP_READY)
#define GIULIA_WGPIO_RELEASE_REQUIRED (GIULIA_WGPIO_ON_REQUIRED | GIULIA_WGPIO_PHY_READY)
#define GIULIA_WGPIO_STOP_REQUIRED (GIULIA_WGPIO_BEGIN_REQUIRED | GIULIA_WGPIO_MSI_STOPPED | GIULIA_WGPIO_PHY_POWER_STOPPED)
struct giulia_wifi_gpio_io {
    void *cookie;
    uint32_t (*read32)(void *, uint32_t offset);
    void (*write32)(void *, uint32_t offset, uint32_t value);
    void (*barrier)(void *);
    void (*delay_us)(void *, uint32_t);
};
struct giulia_wifi_gpio {
    struct giulia_wifi_gpio_io io;
    uint32_t state, saved_cfg[5], saved_io[5], dirty, bt_owned;
    uint32_t sw_ctrl, wlan_sw_ctrl;
    int error;
};
/* Raw TLMM IN_OUT words. Bit 0 samples the input and bit 1 stores the output
 * latch. The six pins are read in sequence and may change between reads. */
struct giulia_wifi_gpio_signals {
    uint32_t wlan_en, sw_ctrl, wlan_sw_ctrl, perst, clkreq, wake;
};
/* Sample during HELD, ON or RELEASED under the caller's existing pin lease.
 * Publish a complete sample only if every read succeeds. */
int giulia_wifi_gpio_snapshot(const struct giulia_wifi_gpio *,
    struct giulia_wifi_gpio_signals *);
/* Caller serializes operations and owns pins 16,94,95,96. Refuses a running
 * WLAN/BT, active pin interrupts or eGPIO ownership. Saves then configures
 * WLAN_EN low, PERST# low, CLKREQ# mux and WAKE# input; holds WLAN low 5 ms.
 * Does not touch BT_EN/SW_CTRL/WLAN_SW_CTRL or their interrupt registers.
 * Unknown/failed writes retain snapshots until stop succeeds.
 */
int giulia_wifi_gpio_begin(struct giulia_wifi_gpio *, uint32_t conditions,
    const struct giulia_wifi_gpio_io *);
/* Optional supply-table owner also takes GPIO17 (BT_EN). Needs a real lease
 * excluding every Bluetooth control client, not just a sampled input low.
 * Refuses a running BT, snapshots its mux/latch, then actively holds reset
 * low alongside WLAN_EN for 5 ms. Other BT/SW_CTRL pins are never changed. */
int giulia_wifi_gpio_begin_with_bt_reset(struct giulia_wifi_gpio *, uint32_t conditions,
    const struct giulia_wifi_gpio_io *);
/* Requires actual rail/AOP completion, not just a plan or requested votes. */
int giulia_wifi_gpio_wlan_on(struct giulia_wifi_gpio *, uint32_t conditions);
/* Enforces >=100 ms PERST# hold and >=1 ms after release before returning. */
int giulia_wifi_gpio_perst_release(struct giulia_wifi_gpio *, uint32_t conditions);
/* Reset first on error/teardown, before link stop. Does not remove WLAN power. */
int giulia_wifi_gpio_perst_assert(struct giulia_wifi_gpio *, uint32_t conditions);
/* After all PCIe/DMA/MSI/PHY users have stopped, keep PERST# asserted and
 * drive WLAN_EN low for5ms, retaining mux/latch snapshots and optional BT
 * reset ownership. This permits supply withdrawal BEFORE GPIO restoration.
 * It never restores an inherited latch or releases BT/SW_CTRL controls. */
int giulia_wifi_gpio_wlan_off_hold(struct giulia_wifi_gpio *, uint32_t conditions);
/* Read-only confirmation before PDC/rail changes: owned WLAN/BT/PERST
 * outputs are held low, SW_CTRL18/19 are GPIO inputs low, IRQ/eGPIO controls
 * inactive. Low input sampling alone never grants the required pin lease. */
int giulia_wifi_gpio_check_held(struct giulia_wifi_gpio *, uint32_t conditions);
int giulia_wifi_gpio_stop(struct giulia_wifi_gpio *, uint32_t conditions);
#endif
