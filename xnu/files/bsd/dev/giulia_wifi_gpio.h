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
    GIULIA_WGPIO_PHY_POWER_STOPPED = 1u << 5
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
    uint32_t state, saved_cfg[4], saved_io[4], dirty;
    uint32_t sw_ctrl, wlan_sw_ctrl;
    int error;
};
/* Caller serializes operations and owns pins 16,94,95,96. Refuses a running
 * WLAN/BT, active pin interrupts or eGPIO ownership. Saves then configures
 * WLAN_EN low, PERST# low, CLKREQ# mux and WAKE# input; holds WLAN low 5 ms.
 * Does not touch BT_EN/SW_CTRL/WLAN_SW_CTRL or their interrupt registers.
 * Unknown/failed writes retain snapshots until stop succeeds.
 */
int giulia_wifi_gpio_begin(struct giulia_wifi_gpio *, uint32_t conditions,
    const struct giulia_wifi_gpio_io *);
/* Requires actual rail/AOP completion, not just a plan or requested votes. */
int giulia_wifi_gpio_wlan_on(struct giulia_wifi_gpio *, uint32_t conditions);
/* Enforces >=100 ms PERST# hold and >=1 ms after release before returning. */
int giulia_wifi_gpio_perst_release(struct giulia_wifi_gpio *, uint32_t conditions);
/* Reset first on error/teardown, before link stop. Does not remove WLAN power. */
int giulia_wifi_gpio_perst_assert(struct giulia_wifi_gpio *, uint32_t conditions);
int giulia_wifi_gpio_stop(struct giulia_wifi_gpio *, uint32_t conditions);
#endif
