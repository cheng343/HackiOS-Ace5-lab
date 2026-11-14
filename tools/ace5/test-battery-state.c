/* Real shared battery-report conversions, suitable for ASAN/UBSAN in cloud CI. */
#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include "../../xnu/files/bsd/sys/redwood_battery.h"

int main(void)
{
    struct redwood_battery_state b = {
        .size = sizeof(b), .percent_hundredths = 7650, .voltage_uv = 4226000,
        .current_ua = -712000, .temperature_hundredths = 2480,
        .present = 1, .external = 1, .status = 1,
        .usb_voltage_uv = 5000000, .usb_current_ua = 2000000
    };
    _Static_assert(sizeof(b) == 60, "battery report ABI size changed");
    _Static_assert(offsetof(struct redwood_battery_state, usb_current_ua) == 56, "battery report ABI fields changed");
    assert(redwood_battery_state_valid(&b));
    assert(!redwood_battery_state_valid(NULL));
    b.current_ua = INT32_MIN;
    b.temperature_hundredths = INT32_MIN;
    assert(redwood_battery_state_valid(&b));
    b.current_ua = INT32_MAX;
    b.temperature_hundredths = INT32_MAX;
    assert(redwood_battery_state_valid(&b));
    b.current_ua = -712000;
    b.temperature_hundredths = 2480;
    assert(redwood_battery_round_hundredths(b.percent_hundredths) == 77);
    assert(redwood_battery_round_hundredths(b.temperature_hundredths) == 25);
    assert(redwood_battery_round_hundredths(-550) == -6);
    assert(redwood_battery_round_hundredths(-500) == -5);
    assert(redwood_battery_round_hundredths(-49) == 0);
    assert(redwood_battery_round_hundredths(-50) == -1);
    assert(redwood_battery_round_hundredths(49) == 0);
    assert(redwood_battery_round_hundredths(50) == 1);
    assert(redwood_battery_round_hundredths(INT32_MIN) == -21474836);
    assert(redwood_battery_round_hundredths(INT32_MAX) == 21474836);
    const int32_t invalid_soc[] = {-1, -100, 10001, INT32_MIN, INT32_MAX};
    for (unsigned i = 0; i < sizeof(invalid_soc) / sizeof(invalid_soc[0]); i++) {
        b.percent_hundredths = invalid_soc[i];
        assert(!redwood_battery_state_valid(&b));
    }
    b.percent_hundredths = 0;
    assert(redwood_battery_state_valid(&b));
    b.percent_hundredths = 10000;
    assert(redwood_battery_state_valid(&b));
    b.voltage_uv = -1000;
    assert(!redwood_battery_state_valid(&b));
    b.voltage_uv = 4226000;
    b.size--;
    assert(!redwood_battery_state_valid(&b));
    b.size++;
    b.usb_voltage_uv = -1000;
    assert(!redwood_battery_state_valid(&b));
    b.usb_voltage_uv = 5000000;
    b.usb_current_ua = -1;
    assert(!redwood_battery_state_valid(&b));
    b.usb_current_ua = 2000000;
    b.present = -1;
    assert(!redwood_battery_state_valid(&b));
    b.present = 1;
    b.external = 2;
    assert(!redwood_battery_state_valid(&b));
    b.external = -2;
    assert(!redwood_battery_state_valid(&b));
    b.external = -1;
    assert(redwood_battery_state_valid(&b));
    assert(!redwood_battery_is_charging(&b));
    b.external = 1;
    assert(redwood_battery_is_charging(&b));
    b.external = 0;
    assert(!redwood_battery_is_charging(&b));
    b.external = 1;
    b.present = 0;
    assert(!redwood_battery_is_charging(&b));
    b.present = 1;
    for (int status = 0; status <= 5; status++) {
        b.status = status;
        assert(redwood_battery_state_valid(&b));
        assert(redwood_battery_is_charging(&b) == (status == 1));
    }
    puts("Battery report tests passed: ABI, invalid/unknown samples, signed rounding and charging flags");
    return 0;
}
