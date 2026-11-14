/* Ace 5 / PKG110 NFG8011B read-only telemetry, independently implemented.
 * Protocol/register facts: OnePlusOSS oplus_hal_bq27541.c/.h and
 * oplus_hal_nfg8011b.h (oneplus/sm8650_b_16.0.0_ace_5), live PKG110 DT.
 * No reset, unseal, calibration, charging, data-flash or fast-sampling commands.
 * set(line, 1) MUST release an open-drain line, never drive it high.
 */
#ifndef GIULIA_BATTERY_BUS_H
#define GIULIA_BATTERY_BUS_H
#include <stdbool.h>
#include <stdint.h>
#include "../sys/redwood_battery.h"

enum { GB_SDA, GB_SCL };
enum { GB_OK = 0, GB_STOPPED = -1, GB_BUSY = -2, GB_TIMEOUT = -3,
       GB_NACK = -4, GB_ARBITRATION = -5, GB_SAMPLE = -6 };
struct gb_bus {
    void *context;
    void (*set)(void *, unsigned, bool);
    bool (*get)(void *, unsigned);
    void (*delay)(void *, unsigned);
    bool (*running)(void *);
};
static int gb_clock_high(struct gb_bus *b)
{
    b->set(b->context, GB_SCL, true);
    /* 5 ms is a bounded failed transaction, not a request to recover/reset a slave. */
    for (unsigned i = 0; i < 1000; i++) {
        if (!b->running(b->context)) return GB_STOPPED;
        if (b->get(b->context, GB_SCL)) {
            b->delay(b->context, 5);
            return GB_OK;
        }
        b->delay(b->context, 5);
    }
    return GB_TIMEOUT;
}
static void gb_clock_low(struct gb_bus *b)
{
    b->set(b->context, GB_SCL, false);
    b->delay(b->context, 5);
}
static int gb_start(struct gb_bus *b)
{
    b->set(b->context, GB_SDA, true);
    b->delay(b->context, 5);
    int rc = gb_clock_high(b);
    if (rc) return rc;
    if (!b->get(b->context, GB_SDA)) return GB_BUSY;
    b->set(b->context, GB_SDA, false);
    b->delay(b->context, 5);
    gb_clock_low(b);
    return GB_OK;
}
static int gb_stop(struct gb_bus *b)
{
    b->set(b->context, GB_SDA, false);
    b->delay(b->context, 5);
    int rc = gb_clock_high(b);
    b->set(b->context, GB_SDA, true);
    b->delay(b->context, 5);
    return rc;
}
static int gb_send(struct gb_bus *b, uint8_t byte)
{
    for (unsigned i = 0; i < 8; i++) {
        bool high = (byte & 0x80) != 0;
        b->set(b->context, GB_SDA, high);
        b->delay(b->context, 5);
        int rc = gb_clock_high(b);
        if (rc) return rc;
        if (high && !b->get(b->context, GB_SDA)) return GB_ARBITRATION;
        gb_clock_low(b);
        byte <<= 1;
    }
    b->set(b->context, GB_SDA, true);
    b->delay(b->context, 5); /* ACK turnaround setup while SCL is still low. */
    int rc = gb_clock_high(b);
    if (rc) return rc;
    bool ack = !b->get(b->context, GB_SDA);
    gb_clock_low(b);
    return ack ? GB_OK : GB_NACK;
}
static int gb_receive(struct gb_bus *b, uint8_t *out, bool last)
{
    uint8_t value = 0;
    b->set(b->context, GB_SDA, true);
    b->delay(b->context, 5); /* Slave data turnaround after the preceding ACK. */
    for (unsigned i = 0; i < 8; i++) {
        int rc = gb_clock_high(b);
        if (rc) return rc;
        value = (uint8_t)((value << 1) | b->get(b->context, GB_SDA));
        gb_clock_low(b);
    }
    b->set(b->context, GB_SDA, last); /* ACK low except NACK on the final byte. */
    b->delay(b->context, 5);
    int rc = gb_clock_high(b);
    if (rc) return rc;
    gb_clock_low(b);
    b->set(b->context, GB_SDA, true);
    *out = value;
    return GB_OK;
}
/* SMBus read-word framing at the verified 7-bit address 0x55. The only byte
 * sent as data is the register selector; the gauge is never configured. */
static int gb_read_word(void *context, uint8_t reg, uint16_t *out)
{
    struct gb_bus *b = context;
    uint8_t lo = 0, hi = 0;
    int rc = gb_start(b);
    if (!rc) rc = gb_send(b, 0xaa);
    if (!rc) rc = gb_send(b, reg);
    if (!rc) rc = gb_start(b);
    if (!rc) rc = gb_send(b, 0xab);
    if (!rc) rc = gb_receive(b, &lo, false);
    if (!rc) rc = gb_receive(b, &hi, true);
    /* Losing arbitration or seeing a busy bus is not permission to issue STOP
     * into another owner's transaction. Timeout/stop likewise only release. */
    if (!rc || rc == GB_NACK) {
        int stopped = gb_stop(b);
        if (!rc) rc = stopped;
    }
    b->set(b->context, GB_SDA, true);
    b->set(b->context, GB_SCL, true);
    if (!rc) *out = (uint16_t)(lo | (uint16_t)hi << 8);
    return rc;
}

typedef int (*gb_reader)(void *, uint8_t, uint16_t *);
/* Standard mode uses 0x08/0x0c. The vendor's 0x1a/0x1c real-time pair is
 * conditional on fast_sampling_enable, which this driver never enables.
 * I/O failures or implausible single-cell readings never become a report.
 */
static int gb_sample(gb_reader read, void *context, struct redwood_battery_state *out)
{
    const uint8_t regs[] = {0x2c, 0x08, 0x0c, 0x06, 0x10, 0x12, 0x2a};
    uint16_t words[7];
    for (unsigned i = 0; i < 7; i++) {
        int rc = read(context, regs[i], &words[i]);
        if (rc) return rc;
    }
    /* The live DT says one cell. Broad rejection limits catch all-zero/all-ff,
     * a missing slave and a wrong register map; they are not charger limits. */
    if (words[0] > 100 || words[1] < 2000 || words[1] > 5000 ||
        words[3] < 2331 || words[3] > 3981 || words[4] == 0xffff ||
        !words[5] || words[5] == 0xffff || words[6] == 0xffff) return GB_SAMPLE;
    int32_t current_ma = words[2] >= 0x8000 ? (int32_t)words[2] - 65536 : words[2];
    struct redwood_battery_state value = {
        .size = sizeof(value), .percent_hundredths = (int32_t)words[0] * 100,
        .voltage_uv = (int32_t)words[1] * 1000, .current_ua = current_ma * 1000,
        .temperature_hundredths = ((int32_t)words[3] - 2731) * 10,
        .present = 1, .external = -1, .status = 0,
        .charge_uah = (int32_t)words[4] * 1000, .full_uah = (int32_t)words[5] * 1000,
        .cycles = words[6]
    };
    /* The vendor returns the NEGATIVE of this signed raw current as mA.
     * Our report ABI instead uses positive charging current, i.e. raw sign.
     * A gauge cannot identify a connected charger; status stays unknown. */
    *out = value;
    return GB_OK;
}
#endif
