/* Shared battery report ABI and conversions; no hardware access. */
#ifndef _SYS_REDWOOD_BATTERY_H_
#define _SYS_REDWOOD_BATTERY_H_

#include <stdint.h>

struct redwood_battery_state {
	uint32_t size;                  /* sizeof(struct redwood_battery_state) */
	int32_t percent_hundredths;     /* 0..10000; negative means no valid sample */
	int32_t voltage_uv;
	int32_t current_ua;             /* positive while the battery takes charge */
	int32_t temperature_hundredths; /* of a degree Celsius; may be negative */
	int32_t present;                /* 1 if there is a battery */
	int32_t external;               /* 1 if external power is connected */
	int32_t status;                 /* Linux POWER_SUPPLY_STATUS_*; 1 is charging */
	int32_t charge_uah, full_uah, design_uah, cycles; /* 0 when not known */
	int32_t usb_type;               /* Linux POWER_SUPPLY_USB_TYPE_* */
	int32_t usb_voltage_uv, usb_current_ua; /* what the port supplies */
};

/* Keep unavailable/error readings out of the power-source properties. In
 * particular, do not turn a negative SOC into a real 0% or negative voltage
 * into a huge unsigned number. No hardware-specific voltage/thermal limits
 * are imposed here. Unknown status codes remain usable as non-charging data.
 */
static inline int
redwood_battery_state_valid(const struct redwood_battery_state *b)
{
	return b != 0 && b->size == sizeof(*b) &&
	    b->percent_hundredths >= 0 && b->percent_hundredths <= 10000 &&
	    b->voltage_uv >= 0 && (b->present == 0 || b->present == 1) &&
	    (b->external == 0 || b->external == 1) &&
	    b->usb_voltage_uv >= 0 && b->usb_current_ua >= 0;
}

/* Nearest whole unit, ties away from zero. Widen before adding/subtracting
 * so the full signed input range is defined, including INT32_MIN/MAX.
 */
static inline int32_t
redwood_battery_round_hundredths(int32_t value)
{
	int64_t wide = value;
	return (int32_t)((wide + (wide < 0 ? -50 : 50)) / 100);
}

static inline int
redwood_battery_is_charging(const struct redwood_battery_state *b)
{
	return b->present && b->external && b->status == 1; /* POWER_SUPPLY_STATUS_CHARGING */
}

#endif
