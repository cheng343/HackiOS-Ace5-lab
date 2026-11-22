/* SPDX-License-Identifier: BSD-2-Clause */
/* Original bounded composition of the Ace5 production GPIO/AOP/PDC/RPMh/
 * rail/CPU-NoC/GCC cores. Per-core files retain their pinned public sources.
 * No resource flag, baseline, firmware handoff or physical NoC proof is
 * invented here. In particular, Android regulator cache is not a baseline.
 */
#include "giulia_wifi_supply.h"
static uintptr_t supply_owner;

static uint32_t required(const struct giulia_wifi_supply *p)
{
    return GIULIA_SUPPLY_REQUIRED | (p->config.conditions & GIULIA_SUPPLY_RCHNG_HANDOFF) |
        (p->config.dt_modes ? GIULIA_SUPPLY_DT_MODE_WRITE_SAFE : 0);
}
static int external(struct giulia_wifi_supply *p, unsigned stage)
{
    p->stage = stage;
    p->external_error = p->config.validate_external(p->config.owner_cookie, required(p), stage);
    return p->external_error ? GIULIA_SUPPLY_EXTERNAL : 0;
}
static uint32_t rail_flags(const struct giulia_wifi_supply_config *c)
{
    return GIULIA_WLAN_RAIL_REQUIRED |
        (c->dt_modes ? GIULIA_WLAN_RAIL_MODE_WRITE_SAFE : 0);
}
static int cpu_lease(void *cookie)
{
    struct giulia_wifi_supply *p = cookie;
    if (giulia_wifi_gpio_check_held(&p->gpio,GIULIA_WGPIO_BEGIN_REQUIRED | GIULIA_WGPIO_BT_CONTROL_EXCLUSIVE)) return -1;
    return external(p, p->stage);
}
static int bad_cleanup(struct giulia_wifi_supply *p, int err, int driver)
{
    p->state = GIULIA_SUPPLY_FAULT; p->cleanup_stage = p->stage;
    p->cleanup_error = err; p->cleanup_driver_error = driver;
    return err;
}
int giulia_wifi_supply_cancel(struct giulia_wifi_supply *p)
{
    int rc, err;
    if (!p) return GIULIA_SUPPLY_ARGUMENT;
    if (p->state == GIULIA_SUPPLY_IDLE || p->state == GIULIA_SUPPLY_STOPPED) return 0;
    if (__atomic_load_n(&supply_owner, __ATOMIC_ACQUIRE) != (uintptr_t)p) return GIULIA_SUPPLY_STATE;
    if (p->state == GIULIA_SUPPLY_HELD) return GIULIA_SUPPLY_RETAINED;
    if (p->state == GIULIA_SUPPLY_HANDED_OFF) return GIULIA_SUPPLY_STATE;
    /* This entry never finishes reset/starts PHY. A later board owner must
     * stop that lifecycle first; reset_attempted alone is historical once
     * power_stop completed. Never cancel a live/failed later power user. */
    if ((p->power.reset_attempted && p->power.state != GIULIA_POWER_STOPPED) ||
        p->power.state == GIULIA_POWER_READY || p->power.state == GIULIA_POWER_PIPE_ON)
        return GIULIA_SUPPLY_STATE;
    p->state = GIULIA_SUPPLY_STOPPING; p->cleanup_error = p->cleanup_driver_error = 0;
#define VALID(s) do { if ((err = external(p,(s)))) return bad_cleanup(p,err,0); } while (0)
#define CLEAN(call) do { rc = (call); if (rc) return bad_cleanup(p,GIULIA_SUPPLY_DRIVER,rc); } while (0)
    if (p->power.state != GIULIA_POWER_IDLE && p->power.state != GIULIA_POWER_STOPPED) {
        VALID(GIULIA_SUPPLY_POWER_STOP);
        CLEAN(giulia_wifi_gpio_perst_assert(&p->gpio, GIULIA_WGPIO_BEGIN_REQUIRED | GIULIA_WGPIO_BT_CONTROL_EXCLUSIVE));
        /* No managed data/CESTA profile has been submitted by this API. */
        CLEAN(giulia_pcie_power_stop(&p->power, GIULIA_POWER_STOP_REQUIRED | GIULIA_POWER_STOP_CESTA_RESOURCES_QUIET));
    }
    if (p->gpio.state != GIULIA_WGPIO_IDLE && p->gpio.state != GIULIA_WGPIO_STOPPED) {
        VALID(GIULIA_SUPPLY_WLAN_OFF);
        CLEAN(giulia_wifi_gpio_wlan_off_hold(&p->gpio, GIULIA_WGPIO_STOP_REQUIRED | GIULIA_WGPIO_BT_CONTROL_EXCLUSIVE));
    }
    if (p->cpu_noc.state != GIULIA_CPU_NOC_IDLE && p->cpu_noc.state != GIULIA_CPU_NOC_STOPPED) {
        VALID(GIULIA_SUPPLY_CPU_STOP);
        rc = giulia_cpu_noc_stop_keep(&p->cpu_noc);
        if (rc == GIULIA_CPU_NOC_RETAINED) p->retained |= GIULIA_SUPPLY_RETAINS_CPU_NOC;
        else if (rc) return bad_cleanup(p,GIULIA_SUPPLY_DRIVER,rc);
    }
    if (p->rails.state != GIULIA_WLAN_RAIL_IDLE && p->rails.state != GIULIA_WLAN_RAIL_STOPPED) {
        VALID(GIULIA_SUPPLY_RAIL_STOP);
        CLEAN(giulia_wifi_gpio_check_held(&p->gpio,GIULIA_WGPIO_BEGIN_REQUIRED | GIULIA_WGPIO_BT_CONTROL_EXCLUSIVE));
        rc = giulia_wlan_rails_stop(&p->rails, p->gpio.state == GIULIA_WGPIO_HELD);
        if (rc == GIULIA_WLAN_RAIL_RETAINED) p->retained |= GIULIA_SUPPLY_RETAINS_RAILS;
        else if (rc) return bad_cleanup(p,GIULIA_SUPPLY_DRIVER,rc);
    }
    if (p->pdc.state != GIULIA_WLAN_PDC_IDLE && p->pdc.state != GIULIA_WLAN_PDC_STOPPED) {
        VALID(GIULIA_SUPPLY_PDC_STOP);
        CLEAN(giulia_wifi_gpio_check_held(&p->gpio,GIULIA_WGPIO_BEGIN_REQUIRED | GIULIA_WGPIO_BT_CONTROL_EXCLUSIVE));
        rc = giulia_wlan_pdc_stop(&p->pdc, GIULIA_WLAN_PDC_REQUIRED);
        if (rc == GIULIA_WLAN_PDC_RETAINED) p->retained |= GIULIA_SUPPLY_RETAINS_PDC;
        else if (rc) return bad_cleanup(p,GIULIA_SUPPLY_DRIVER,rc);
    }
    if (p->aop.owned) {
        VALID(GIULIA_SUPPLY_AOP_STOP);
        CLEAN(giulia_aop_close(&p->aop));
    }
    if (p->retained) { p->state = GIULIA_SUPPLY_HELD; return GIULIA_SUPPLY_RETAINED; }
    if (p->gpio.state != GIULIA_WGPIO_IDLE && p->gpio.state != GIULIA_WGPIO_STOPPED) {
        VALID(GIULIA_SUPPLY_GPIO_STOP);
        CLEAN(giulia_wifi_gpio_stop(&p->gpio, GIULIA_WGPIO_STOP_REQUIRED | GIULIA_WGPIO_BT_CONTROL_EXCLUSIVE));
    }
    uintptr_t expected = (uintptr_t)p;
    if (!__atomic_compare_exchange_n(&supply_owner, &expected, 0, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
        return bad_cleanup(p,GIULIA_SUPPLY_STATE,0);
    p->state = GIULIA_SUPPLY_STOPPED;
    return 0;
#undef VALID
#undef CLEAN
}
int giulia_wifi_supply_handoff_host(struct giulia_wifi_supply *p)
{
    int err;
    if (!p) return GIULIA_SUPPLY_ARGUMENT;
    if (p->state != GIULIA_SUPPLY_HOST_PREPARED || p->power.state != GIULIA_POWER_HOST_PREPARED ||
        p->power.reset_attempted || __atomic_load_n(&supply_owner, __ATOMIC_ACQUIRE) != (uintptr_t)p)
        return GIULIA_SUPPLY_STATE;
    if ((err = external(p,GIULIA_SUPPLY_HOST_HANDOFF))) return err;
    p->state = GIULIA_SUPPLY_HANDED_OFF;
    return 0;
}
static int start_failed(struct giulia_wifi_supply *p, int err, int driver)
{
    p->error = err; p->driver_error = driver; p->failed_stage = p->stage;
    p->state = GIULIA_SUPPLY_FAULT;
    int cleanup = giulia_wifi_supply_cancel(p);
    if (cleanup < 0 && !p->cleanup_error) { p->cleanup_error = cleanup; p->cleanup_stage = p->stage; }
    return err;
}
static int valid_io(const struct giulia_wifi_supply_config *c)
{
    return c && c->validate_external && c->gpio.read32 && c->gpio.write32 &&
        c->gpio.barrier && c->gpio.delay_us && c->aop.read32 && c->aop.write32 &&
        c->aop.barrier && c->aop.delay_us && c->aop.kick && c->rpmh.read32 &&
        c->rpmh.write32 && c->rpmh.barrier && c->rpmh.delay_us && c->power.read32 &&
        c->power.write32 && c->power.barrier && c->power.delay_us;
}
int giulia_wifi_supply_prepare(struct giulia_wifi_supply *p, const struct giulia_wifi_supply_config *c)
{
    int rc, err;
    uintptr_t expected = 0;
    struct giulia_cpu_noc_plan plan;
    if (!p || !valid_io(c) || !c->rail_baseline || c->pdc_bytes != 508 ||
        c->dt_modes > 1 || c->aop_policy > GIULIA_AOP_OPEN_ON_INHERITED_LINK ||
        c->rail_policy > GIULIA_WLAN_KEEP_VOTES)
        return GIULIA_SUPPLY_ARGUMENT;
    if (p->state != GIULIA_SUPPLY_IDLE && p->state != GIULIA_SUPPLY_STOPPED) return GIULIA_SUPPLY_STATE;
    uint32_t need = GIULIA_SUPPLY_REQUIRED | (c->conditions & GIULIA_SUPPLY_RCHNG_HANDOFF) |
        (c->dt_modes ? GIULIA_SUPPLY_DT_MODE_WRITE_SAFE : 0);
    if ((c->conditions & need) != need || c->allow_retained_cpu_floor != 1) return GIULIA_SUPPLY_PREREQUISITE;
    if (c->validate_external(c->owner_cookie,need,GIULIA_SUPPLY_INPUTS)) return GIULIA_SUPPLY_EXTERNAL;
    if ((rc = giulia_wlan_pdc_check(c->pdc_table,c->pdc_bytes,c->pdc_baseline,c->pdc_policy)) ||
        (rc = giulia_wlan_rails_check(c->db,c->db_bytes,c->db_version,c->rail_baseline,
            rail_flags(c),(enum giulia_wlan_rail_policy)c->rail_policy,c->dt_modes)) ||
        (rc = giulia_cpu_noc_plan(c->db,c->db_bytes,c->db_version,&plan))) {
        p->error = GIULIA_SUPPLY_DRIVER; p->driver_error = rc; p->failed_stage = GIULIA_SUPPLY_INPUTS;
        return GIULIA_SUPPLY_DRIVER;
    }
    struct giulia_wlan_rail_baseline rails[7];
    struct giulia_wlan_pdc_baseline pdc = {0};
    uint8_t table[508];
    __builtin_memcpy(rails,c->rail_baseline,sizeof(rails));
    __builtin_memcpy(table,c->pdc_table,sizeof(table));
    if (c->pdc_baseline) pdc = *c->pdc_baseline;
    if (!__atomic_compare_exchange_n(&supply_owner, &expected, (uintptr_t)p, 0,
        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return GIULIA_SUPPLY_STATE;
    struct giulia_wifi_supply_config copy = *c;
    *p = (struct giulia_wifi_supply){0}; p->config = copy; c = &p->config;
    __builtin_memcpy(p->input_rails,rails,sizeof(rails));
    __builtin_memcpy(p->input_table,table,sizeof(table)); p->input_pdc = pdc;
    p->config.rail_baseline = p->input_rails; p->config.pdc_table = p->input_table;
    p->config.pdc_baseline = copy.pdc_baseline ? &p->input_pdc : 0;
    p->state = GIULIA_SUPPLY_PREPARING;
#define STEP(s) do { if ((err = external(p,(s)))) return start_failed(p,err,0); } while (0)
#define CALL(call) do { if ((rc = (call))) return start_failed(p,GIULIA_SUPPLY_DRIVER,rc); } while (0)
#define EXPECT(v) do { if (!(v)) return start_failed(p,GIULIA_SUPPLY_CONTRACT,0); } while (0)
    STEP(GIULIA_SUPPLY_GPIO);
    CALL(giulia_wifi_gpio_begin_with_bt_reset(&p->gpio,GIULIA_WGPIO_BEGIN_REQUIRED | GIULIA_WGPIO_BT_CONTROL_EXCLUSIVE,&c->gpio));
    EXPECT(p->gpio.state == GIULIA_WGPIO_HELD && p->gpio.bt_owned);
    CALL(giulia_rpmh_init(&p->rpmh,&c->rpmh,GIULIA_RPMH_REQUIRED));
    STEP(GIULIA_SUPPLY_AOP);
    CALL(giulia_aop_open(&p->aop,&c->aop,GIULIA_AOP_MSGRAM_SIZE,
        GIULIA_AOP_REQUIRED | GIULIA_AOP_INHERITED_CHANNEL_LEASE,c->aop_policy));
    EXPECT(p->aop.state == GIULIA_AOP_OPEN && p->aop.owned);
    STEP(GIULIA_SUPPLY_PDC);
    CALL(giulia_wifi_gpio_check_held(&p->gpio,GIULIA_WGPIO_BEGIN_REQUIRED | GIULIA_WGPIO_BT_CONTROL_EXCLUSIVE));
    CALL(giulia_wlan_pdc_start(&p->pdc,&p->aop,c->pdc_table,c->pdc_bytes,c->pdc_baseline,
        GIULIA_WLAN_PDC_REQUIRED,c->pdc_policy));
    EXPECT(p->pdc.state == GIULIA_WLAN_PDC_DELIVERED && p->pdc.delivered == GIULIA_WLAN_PDC_ALL);
    STEP(GIULIA_SUPPLY_RAILS);
    CALL(giulia_wifi_gpio_check_held(&p->gpio,GIULIA_WGPIO_BEGIN_REQUIRED | GIULIA_WGPIO_BT_CONTROL_EXCLUSIVE));
    if (c->dt_modes) {
        CALL(giulia_wlan_rails_start_with_dt_modes(&p->rails,&p->rpmh,c->db,c->db_bytes,c->db_version,
            c->rail_baseline,rail_flags(c),(enum giulia_wlan_rail_policy)c->rail_policy));
    } else {
        CALL(giulia_wlan_rails_start(&p->rails,&p->rpmh,c->db,c->db_bytes,c->db_version,
            c->rail_baseline,rail_flags(c),(enum giulia_wlan_rail_policy)c->rail_policy));
    }
    EXPECT(p->rails.state == GIULIA_WLAN_RAIL_ACTIVE && p->rails.completed_mask == 0x7fu);
    STEP(GIULIA_SUPPLY_CPU_NOC);
    struct giulia_cpu_noc_lease cpu = {p,cpu_lease};
    CALL(giulia_cpu_noc_start_keep(&p->cpu_noc,&p->rpmh,&cpu,c->db,c->db_bytes,c->db_version,GIULIA_CPU_NOC_REQUIRED));
    EXPECT(p->cpu_noc.state == GIULIA_CPU_NOC_ACTIVE && p->cpu_noc.completed);
    STEP(GIULIA_SUPPLY_WLAN_ON);
    CALL(giulia_wifi_gpio_wlan_on(&p->gpio,GIULIA_WGPIO_ON_REQUIRED | GIULIA_WGPIO_BT_CONTROL_EXCLUSIVE));
    EXPECT(p->gpio.state == GIULIA_WGPIO_ON);
    STEP(GIULIA_SUPPLY_HOST);
    CALL(giulia_pcie_power_prepare_host(&p->power,&c->power,
        GIULIA_POWER_HOST_REQUIRED | GIULIA_POWER_CESTA_COORDINATED |
        ((c->conditions & GIULIA_SUPPLY_RCHNG_HANDOFF) ? GIULIA_POWER_RCHNG_HANDOFF : 0) |
        ((c->conditions & GIULIA_SUPPLY_CESTA_PIPE_OBSERVE) ? GIULIA_POWER_CESTA_PIPE_OBSERVE : 0)));
    EXPECT(p->power.state == GIULIA_POWER_HOST_PREPARED);
    p->state = GIULIA_SUPPLY_HOST_PREPARED;
    return 0;
#undef STEP
#undef CALL
#undef EXPECT
}
