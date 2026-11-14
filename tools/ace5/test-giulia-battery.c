/* Real transport/sample implementation, fake open-drain slave; no hardware. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../xnu/files/bsd/dev/giulia_battery_bus.h"
struct fake_bus {
    bool sda[128], release[2], alive;
    unsigned count, at, starts, stops, scl_reads, stretch_reads, us;
    bool setup_pending;
    unsigned setup_at;
};
static void set(void *p, unsigned line, bool release)
{
    struct fake_bus *f = p;
    if (line == GB_SDA && !f->release[GB_SCL]) {
        f->setup_pending = true;
        f->setup_at = f->us;
    }
    if (line == GB_SCL && release && !f->release[GB_SCL]) {
        assert(!f->setup_pending || f->us - f->setup_at >= 5);
        f->setup_pending = false;
    }
    if (line == GB_SDA && f->release[GB_SCL] && f->release[GB_SDA] != release) {
        if (release) f->stops++; else f->starts++;
    }
    f->release[line] = release;
}
static bool get(void *p, unsigned line)
{
    struct fake_bus *f = p;
    if (line == GB_SCL) return ++f->scl_reads > f->stretch_reads;
    assert(f->at < f->count);
    return f->sda[f->at++];
}
static void delay(void *p, unsigned us) { ((struct fake_bus *)p)->us += us; }
static bool running(void *p) { return ((struct fake_bus *)p)->alive; }
static void bit(struct fake_bus *f, bool v) { assert(f->count < 128); f->sda[f->count++] = v; }
static void sent(struct fake_bus *f, uint8_t byte, bool ack)
{
    for (unsigned i = 0; i < 8; i++, byte <<= 1) if (byte & 0x80) bit(f, true);
    bit(f, !ack);
}
static void received(struct fake_bus *f, uint8_t byte)
{
    for (unsigned i = 0; i < 8; i++, byte <<= 1) bit(f, (byte & 0x80) != 0);
}
static struct gb_bus bus(struct fake_bus *f)
{
    f->release[0] = f->release[1] = f->alive = true;
    return (struct gb_bus){f, set, get, delay, running};
}
struct fake_gauge { uint16_t word[7]; unsigned calls; int fail; };
static int read_gauge(void *p, uint8_t reg, uint16_t *out)
{
    static const uint8_t expected[] = {0x2c, 0x08, 0x0c, 0x06, 0x10, 0x12, 0x2a};
    struct fake_gauge *g = p;
    assert(g->calls < 7 && reg == expected[g->calls]);
    if ((int)g->calls++ == g->fail) return GB_NACK;
    *out = g->word[g->calls - 1];
    return 0;
}
int main(void)
{
    struct fake_bus f = {0};
    struct gb_bus b = bus(&f);
    bit(&f, true); sent(&f, 0xaa, true); sent(&f, 0x08, true);
    bit(&f, true); sent(&f, 0xab, true); received(&f, 0x34); received(&f, 0x12);
    f.stretch_reads = 3;
    uint16_t word = 0xbeef;
    assert(gb_read_word(&b, 0x08, &word) == 0 && word == 0x1234);
    assert(f.at == f.count && f.starts == 2 && f.stops == 1);
    assert(f.release[0] && f.release[1]);

    memset(&f, 0, sizeof(f)); b = bus(&f);
    bit(&f, true); sent(&f, 0xaa, false);
    word = 0xbeef;
    assert(gb_read_word(&b, 0x08, &word) == GB_NACK && word == 0xbeef);
    assert(f.stops == 1 && f.at == f.count);

    memset(&f, 0, sizeof(f)); b = bus(&f);
    bit(&f, true); bit(&f, false); /* Another master holds SDA low while we release a 1. */
    assert(gb_read_word(&b, 0x08, &word) == GB_ARBITRATION);
    assert(f.stops == 0 && f.release[0] && f.release[1]);

    memset(&f, 0, sizeof(f)); b = bus(&f); bit(&f, false);
    assert(gb_read_word(&b, 0x08, &word) == GB_BUSY && f.stops == 0);
    memset(&f, 0, sizeof(f)); b = bus(&f); f.stretch_reads = 100000;
    assert(gb_read_word(&b, 0x08, &word) == GB_TIMEOUT);
    assert(f.us == 5005 && f.scl_reads == 1000 && f.stops == 0);
    memset(&f, 0, sizeof(f)); b = bus(&f); f.alive = false;
    assert(gb_read_word(&b, 0x08, &word) == GB_STOPPED && f.scl_reads == 0);

    /* Synthetic readings, unrelated to any user's battery. Raw current sign
     * is opposite Oplus current_now in mA; no production defaults exist. */
    struct fake_gauge g = {{43, 3997, 123, 2956, 2500, 6000, 12}, 0, -1};
    struct redwood_battery_state report;
    memset(&report, 0xa5, sizeof(report));
    assert(gb_sample(read_gauge, &g, &report) == 0 && g.calls == 7);
    assert(report.percent_hundredths == 4300 && report.voltage_uv == 3997000);
    assert(report.current_ua == 123000 && report.temperature_hundredths == 2250);
    assert(report.charge_uah == 2500000 && report.full_uah == 6000000 && report.cycles == 12);
    assert(report.external == -1 && report.status == 0 && redwood_battery_state_valid(&report));
    assert(!redwood_battery_is_charging(&report));
    g.calls = 0; g.word[2] = (uint16_t)-123;
    assert(gb_sample(read_gauge, &g, &report) == 0 && report.current_ua == -123000);
    struct redwood_battery_state previous = report;
    for (int fail = 0; fail < 7; fail++) {
        g.calls = 0; g.fail = fail;
        assert(gb_sample(read_gauge, &g, &report) == GB_NACK);
        assert(!memcmp(&report, &previous, sizeof(report)));
    }
    g.fail = -1; g.calls = 0; g.word[0] = 101;
    assert(gb_sample(read_gauge, &g, &report) == GB_SAMPLE);
    assert(!memcmp(&report, &previous, sizeof(report)));
    g.calls = 0; memset(g.word, 0xff, sizeof(g.word));
    assert(gb_sample(read_gauge, &g, &report) == GB_SAMPLE);
    g.calls = 0; memset(g.word, 0, sizeof(g.word));
    assert(gb_sample(read_gauge, &g, &report) == GB_SAMPLE);
    puts("PASS: actual GPIO I2C protocol framing, stretch/NACK/arbitration/STOP, NFG sampling and unknown charger");
    return 0;
}
