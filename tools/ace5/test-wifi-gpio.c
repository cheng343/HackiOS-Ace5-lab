/* SPDX-License-Identifier: BSD-2-Clause */
#include "giulia_wifi_gpio.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define PIN(p,r) ((p) * 0x1000u + (r))
struct mock {
    uint32_t regs[97][3], original[97][3];
    unsigned reads, writes, delay_us, delay100ms, ignore_nth, bt_allowed;
    uint32_t ignore, bad_read;
};
static uint32_t rd(void *cookie, uint32_t off)
{
    struct mock *m = cookie;
    assert(off / 0x1000u < 97 && off % 0x1000u <= 8 && !(off & 3u));
    ++m->reads;
    if (off == m->bad_read) return UINT32_MAX;
    return m->regs[off / 0x1000u][(off % 0x1000u) / 4];
}
static void wr(void *cookie, uint32_t off, uint32_t val)
{
    struct mock *m = cookie;
    unsigned pin = off / 0x1000u, reg = off % 0x1000u / 4;
    assert((pin == 16 || pin == 94 || pin == 95 || pin == 96 || (pin == 17 && m->bt_allowed)) && reg < 2);
    assert(((val ^ m->regs[pin][reg]) & ~(reg ? 2u : 0x3ffu)) == 0);
    ++m->writes;
    if (off == m->ignore || m->writes == m->ignore_nth) return;
    if (pin == 94 && reg == 1 && (val & 2u) && (m->regs[94][0] & (1u << 9))) {
        /* A live output release needs the delay. Restoring an original high
         * latch while OE is cleared is not a new endpoint release.
         */
        assert(m->delay100ms && (m->regs[16][1] & 2u));
    }
    m->regs[pin][reg] = val;
}
static void fence(void *cookie) { (void)cookie; }
static void delay(void *cookie, uint32_t us)
{
    struct mock *m = cookie;
    assert(us == 1000 || us == 5000 || us == 100000);
    m->delay_us += us;
    if (us == 100000) {
        assert(!(m->regs[94][1] & 2u)); ++m->delay100ms;
    }
}
static void setup(struct mock *m, struct giulia_wifi_gpio *g)
{
    memset(m, 0, sizeof(*m)); memset(g, 0, sizeof(*g));
    m->ignore = m->bad_read = UINT32_MAX;
    for (unsigned pin = 0; pin < 97; ++pin) {
        m->regs[pin][0] = 0xab000000;
        m->regs[pin][1] = 0xcd000000;
    }
    memcpy(m->original, m->regs, sizeof(m->regs));
}
static void begin(struct mock *m, struct giulia_wifi_gpio *g)
{
    struct giulia_wifi_gpio_io io = {m, rd, wr, fence, delay};
    assert(!giulia_wifi_gpio_begin(g, GIULIA_WGPIO_BEGIN_REQUIRED, &io));
}
static void stop(struct mock *m, struct giulia_wifi_gpio *g)
{
    assert(!giulia_wifi_gpio_stop(g, GIULIA_WGPIO_STOP_REQUIRED));
    assert(g->state == GIULIA_WGPIO_STOPPED && !g->dirty);
    assert(!memcmp(m->regs, m->original, sizeof(m->regs)));
}
static void bluetooth_reset(void)
{
    struct mock m;
    struct giulia_wifi_gpio g;
    struct giulia_wifi_gpio_io io = {&m, rd, wr, fence, delay};
    unsigned flags = GIULIA_WGPIO_BEGIN_REQUIRED | GIULIA_WGPIO_BT_CONTROL_EXCLUSIVE;
    unsigned stop_flags = GIULIA_WGPIO_STOP_REQUIRED | GIULIA_WGPIO_BT_CONTROL_EXCLUSIVE;
    setup(&m, &g);
    assert(giulia_wifi_gpio_begin_with_bt_reset(&g, GIULIA_WGPIO_BEGIN_REQUIRED, &io) == GIULIA_WGPIO_PREREQUISITE);
    assert(!m.reads && !m.writes);
    for (unsigned bad = 0; bad < 3; bad++) {
        setup(&m, &g); m.bt_allowed = 1;
        if (!bad) m.regs[17][1] |= 1;
        if (bad == 1) m.regs[17][0] |= 1u << 12;
        if (bad == 2) m.regs[17][2] = 1;
        assert(giulia_wifi_gpio_begin_with_bt_reset(&g, flags, &io) == GIULIA_WGPIO_BUSY);
        assert(!m.writes);
    }
    setup(&m, &g); m.bt_allowed = 1;
    assert(!giulia_wifi_gpio_begin_with_bt_reset(&g, flags, &io));
    assert(g.bt_owned && g.state == GIULIA_WGPIO_HELD && m.delay_us == 5000);
    assert((m.regs[17][0] & 0x3ff) == 0x201 && !(m.regs[17][1] & 2));
    unsigned start_writes = m.writes;
    assert(giulia_wifi_gpio_stop(&g, GIULIA_WGPIO_STOP_REQUIRED) == GIULIA_WGPIO_PREREQUISITE && m.writes == start_writes);
    assert(!giulia_wifi_gpio_stop(&g, stop_flags));
    unsigned stop_writes = m.writes - start_writes;
    assert(!g.bt_owned && !g.dirty && !memcmp(m.regs, m.original, sizeof(m.regs)));
    for (unsigned nth = 1; nth <= start_writes; nth++) {
        setup(&m, &g); m.bt_allowed = 1; m.ignore_nth = nth;
        int rc = giulia_wifi_gpio_begin_with_bt_reset(&g, flags, &io);
        assert(rc == 0 || rc == GIULIA_WGPIO_READBACK);
        m.ignore_nth = 0;
        assert(!giulia_wifi_gpio_stop(&g, stop_flags));
        assert(!g.dirty && !g.bt_owned && !memcmp(m.regs, m.original, sizeof(m.regs)));
    }
    for (unsigned nth = 1; nth <= stop_writes; nth++) {
        setup(&m, &g); m.bt_allowed = 1;
        assert(!giulia_wifi_gpio_begin_with_bt_reset(&g, flags, &io));
        m.ignore_nth = m.writes + nth;
        int rc = giulia_wifi_gpio_stop(&g, stop_flags);
        assert(rc == 0 || rc == GIULIA_WGPIO_READBACK);
        m.ignore_nth = 0;
        assert(!giulia_wifi_gpio_stop(&g, stop_flags));
        assert(!g.dirty && !g.bt_owned && !memcmp(m.regs, m.original, sizeof(m.regs)));
    }
    puts("Ace5 Wi-Fi GPIO: explicit BT lease, held reset and every partial restore passed");
}
static void off_hold(void)
{
    struct mock m;struct giulia_wifi_gpio g;
    struct giulia_wifi_gpio_io io={&m,rd,wr,fence,delay};
    unsigned begin_flags=GIULIA_WGPIO_BEGIN_REQUIRED|GIULIA_WGPIO_BT_CONTROL_EXCLUSIVE;
    unsigned stop_flags=GIULIA_WGPIO_STOP_REQUIRED|GIULIA_WGPIO_BT_CONTROL_EXCLUSIVE;
    setup(&m,&g);m.bt_allowed=1;
    assert(!giulia_wifi_gpio_begin_with_bt_reset(&g,begin_flags,&io));
    unsigned before=m.writes;
    assert(!giulia_wifi_gpio_check_held(&g,begin_flags)&&m.writes==before);
    m.regs[18][1]|=1;
    assert(giulia_wifi_gpio_check_held(&g,begin_flags)==GIULIA_WGPIO_BUSY&&m.writes==before);
    m.regs[18][1]&=~1u;m.regs[19][0]|=1u<<9;
    assert(giulia_wifi_gpio_check_held(&g,begin_flags)==GIULIA_WGPIO_BUSY&&m.writes==before);
    m.regs[19][0]&=~(1u<<9);m.bad_read=PIN(94,0);
    assert(giulia_wifi_gpio_check_held(&g,begin_flags)==GIULIA_WGPIO_READBACK&&m.writes==before);
    m.bad_read=UINT32_MAX;
    assert(!giulia_wifi_gpio_wlan_on(&g,GIULIA_WGPIO_ON_REQUIRED|GIULIA_WGPIO_BT_CONTROL_EXCLUSIVE));
    before=m.writes;
    assert(giulia_wifi_gpio_wlan_off_hold(&g,begin_flags)==GIULIA_WGPIO_PREREQUISITE&&m.writes==before);
    assert(!giulia_wifi_gpio_wlan_off_hold(&g,stop_flags));
    assert(g.state==GIULIA_WGPIO_HELD&&g.bt_owned&&g.dirty==31);
    assert(!(m.regs[16][1]&2)&&!(m.regs[94][1]&2));
    assert(!giulia_wifi_gpio_check_held(&g,begin_flags));
    assert(!giulia_wifi_gpio_stop(&g,stop_flags));
    assert(!memcmp(m.regs,m.original,sizeof(m.regs)));
    puts("Ace5 Wi-Fi GPIO: WLAN off retains reset snapshots; live controls checked without writes");
}
int main(void)
{
    struct mock m;
    struct giulia_wifi_gpio g;
    struct giulia_wifi_gpio_io io = {&m, rd, wr, fence, delay};
    bluetooth_reset();
    off_hold();
    setup(&m, &g);
    assert(giulia_wifi_gpio_begin(&g, 0, &io) == GIULIA_WGPIO_PREREQUISITE);
    assert(!m.reads && !m.writes);
    setup(&m, &g); m.regs[17][1] |= 1;
    assert(giulia_wifi_gpio_begin(&g, GIULIA_WGPIO_BEGIN_REQUIRED, &io) == GIULIA_WGPIO_BUSY);
    assert(!m.writes);
    setup(&m, &g); m.regs[16][1] |= 2;
    assert(giulia_wifi_gpio_begin(&g, GIULIA_WGPIO_BEGIN_REQUIRED, &io) == GIULIA_WGPIO_BUSY);
    assert(!m.writes);
    setup(&m, &g); m.regs[95][2] = 1;
    assert(giulia_wifi_gpio_begin(&g, GIULIA_WGPIO_BEGIN_REQUIRED, &io) == GIULIA_WGPIO_BUSY);
    assert(!m.writes);
    setup(&m, &g); m.regs[96][0] |= 1u << 12;
    assert(giulia_wifi_gpio_begin(&g, GIULIA_WGPIO_BEGIN_REQUIRED, &io) == GIULIA_WGPIO_BUSY);
    assert(!m.writes);
    setup(&m, &g); m.bad_read = PIN(94, 0);
    assert(giulia_wifi_gpio_begin(&g, GIULIA_WGPIO_BEGIN_REQUIRED, &io) == GIULIA_WGPIO_READBACK);
    assert(!m.writes);
    setup(&m, &g); g.state = UINT32_MAX;
    assert(giulia_wifi_gpio_perst_assert(&g, GIULIA_WGPIO_BEGIN_REQUIRED) == GIULIA_WGPIO_STATE);
    assert(giulia_wifi_gpio_stop(&g, GIULIA_WGPIO_STOP_REQUIRED) == GIULIA_WGPIO_STATE);
    assert(!m.reads && !m.writes);

    setup(&m, &g); begin(&m, &g);
    assert(g.state == GIULIA_WGPIO_HELD && m.delay_us == 5000);
    assert((m.regs[16][0] & 0x3ffu) == 0x201 && !(m.regs[16][1] & 2));
    assert((m.regs[95][0] & 0x3ffu) == 7 && (m.regs[96][0] & 0x3ffu) == 3);
    unsigned writes = m.writes;
    assert(giulia_wifi_gpio_wlan_on(&g, GIULIA_WGPIO_BEGIN_REQUIRED) == GIULIA_WGPIO_PREREQUISITE);
    assert(m.writes == writes);
    assert(!giulia_wifi_gpio_wlan_on(&g, GIULIA_WGPIO_ON_REQUIRED));
    assert(g.state == GIULIA_WGPIO_ON && (m.regs[16][0] & 0x3ffu) == 0x3c3);
    assert(giulia_wifi_gpio_perst_release(&g, GIULIA_WGPIO_ON_REQUIRED) == GIULIA_WGPIO_PREREQUISITE);
    assert(!giulia_wifi_gpio_perst_release(&g, GIULIA_WGPIO_RELEASE_REQUIRED));
    assert(g.state == GIULIA_WGPIO_RELEASED && m.delay_us == 107000);
    assert(!giulia_wifi_gpio_perst_assert(&g, GIULIA_WGPIO_BEGIN_REQUIRED));
    assert(!(m.regs[94][1] & 2u));
    writes = m.writes;
    assert(giulia_wifi_gpio_stop(&g, 0) == GIULIA_WGPIO_PREREQUISITE && m.writes == writes);
    stop(&m, &g);
    writes = m.writes;
    assert(!giulia_wifi_gpio_stop(&g, 0) && m.writes == writes);

    setup(&m, &g); m.ignore = PIN(95, 0);
    assert(giulia_wifi_gpio_begin(&g, GIULIA_WGPIO_BEGIN_REQUIRED, &io) == GIULIA_WGPIO_READBACK);
    assert(g.state == GIULIA_WGPIO_FAULT);
    m.ignore = UINT32_MAX; stop(&m, &g);
    /* All partially acquired pin combinations keep a complete snapshot. */
    for (unsigned nth = 1; nth <= 6; ++nth) {
        int result;
        setup(&m, &g); m.ignore_nth = nth;
        result = giulia_wifi_gpio_begin(&g, GIULIA_WGPIO_BEGIN_REQUIRED, &io);
        assert(result == 0 || result == GIULIA_WGPIO_READBACK);
        if (result) assert(g.state == GIULIA_WGPIO_FAULT);
        m.ignore_nth = 0; stop(&m, &g);
    }
    /* Restore a pre-existing high PERST latch without treating it as a new
     * live release. WLAN was off at admission and remains off on teardown.
     */
    setup(&m, &g);
    m.regs[94][0] |= 1u << 9; m.regs[94][1] |= 2u;
    memcpy(m.original, m.regs, sizeof(m.regs));
    begin(&m, &g); stop(&m, &g);
    assert(!m.delay100ms);
    /* Security/reserved fields can change outside our owned low field mask.
     * Teardown preserves their current values, not old whole-register words.
     */
    setup(&m, &g); begin(&m, &g);
    m.regs[95][0] |= 1u << 20; m.original[95][0] |= 1u << 20;
    m.regs[96][1] |= 1u << 21; m.original[96][1] |= 1u << 21;
    stop(&m, &g);
    setup(&m, &g); begin(&m, &g); m.bad_read = PIN(18, 4);
    assert(giulia_wifi_gpio_wlan_on(&g, GIULIA_WGPIO_ON_REQUIRED) == GIULIA_WGPIO_READBACK);
    assert(g.state == GIULIA_WGPIO_FAULT && (m.regs[16][1] & 2u));
    m.bad_read = UINT32_MAX; stop(&m, &g);
    /* Drop each teardown write once; retry must still restore the original
     * snapshots even if earlier pins were already restored successfully.
     */
    setup(&m, &g); begin(&m, &g); writes = m.writes;
    stop(&m, &g);
    unsigned teardown_writes = m.writes - writes;
    for (unsigned nth = 1; nth <= teardown_writes; ++nth) {
        int result;
        setup(&m, &g); begin(&m, &g); m.ignore_nth = m.writes + nth;
        result = giulia_wifi_gpio_stop(&g, GIULIA_WGPIO_STOP_REQUIRED);
        assert(result == 0 || result == GIULIA_WGPIO_READBACK);
        m.ignore_nth = 0; stop(&m, &g);
    }
    setup(&m, &g); begin(&m, &g); m.ignore = PIN(16, 4);
    assert(giulia_wifi_gpio_wlan_on(&g, GIULIA_WGPIO_ON_REQUIRED) == GIULIA_WGPIO_READBACK);
    assert(g.state == GIULIA_WGPIO_FAULT);
    m.ignore = UINT32_MAX; stop(&m, &g);
    setup(&m, &g); begin(&m, &g);
    assert(!giulia_wifi_gpio_wlan_on(&g, GIULIA_WGPIO_ON_REQUIRED));
    m.ignore = PIN(16, 4);
    assert(giulia_wifi_gpio_stop(&g, GIULIA_WGPIO_STOP_REQUIRED) == GIULIA_WGPIO_READBACK);
    assert(g.state == GIULIA_WGPIO_FAULT && g.dirty);
    m.ignore = UINT32_MAX; stop(&m, &g);
    puts("Ace5 Wi-Fi GPIO: board wiring, WLAN/PERST timing and failed teardown retry passed");
    return 0;
}
