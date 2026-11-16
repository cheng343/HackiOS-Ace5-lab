/* SPDX-License-Identifier: BSD-2-Clause */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../xnu/files/bsd/dev/giulia_kiwi_reg.c"

struct mock_bar {
    uint32_t window_reg;
    uint32_t low_word;
    uint32_t high_word;
    uint32_t last_off;
    uint32_t fail_read;
    uint32_t bad_window_rb;
};

static int m_read(void *cookie, uint32_t off, uint32_t *val)
{
    struct mock_bar *b = cookie;
    if (b->fail_read) return -1;
    if (off == GIULIA_KIWI_WINDOW_REG) {
        *val = b->bad_window_rb ? 0u : b->window_reg;
        return 0;
    }
    b->last_off = off;
    if (off == GIULIA_KIWI_SOC_INFO_REG) { *val = b->low_word; return 0; }
    *val = off >= GIULIA_KIWI_MAX_UNWINDOWED ? b->high_word : 0x12345678u;
    return 0;
}

static int m_write(void *cookie, uint32_t off, uint32_t val)
{
    struct mock_bar *b = cookie;
    if (off == GIULIA_KIWI_WINDOW_REG) { b->window_reg = val; return 0; }
    b->last_off = off;
    if (off >= GIULIA_KIWI_MAX_UNWINDOWED) b->high_word = val;
    else b->low_word = val;
    return 0;
}

int main(void)
{
    struct mock_bar b = {0};
    struct giulia_kiwi_bar_io io = {&b, m_read, m_write, 0x200000u};
    struct giulia_kiwi_soc_info info = {0};
    uint32_t val = 0;

    /* Direct unwindowed read */
    assert(giulia_kiwi_reg_read(&io, 0x100u, &val) == GIULIA_KIWI_OK);
    assert(val == 0x12345678u && b.last_off == 0x100u && b.window_reg == 0u);

    /* Windowed high SoC write + read: 0x00a00004 -> win=(0xa00004>>19)=0x14 */
    assert(giulia_kiwi_reg_write(&io, 0x00a00004u, 0xdeadbeefu) == GIULIA_KIWI_OK);
    assert(b.window_reg == (GIULIA_KIWI_WINDOW_ENABLE | 0x14u));
    assert(b.last_off == (GIULIA_KIWI_MAX_UNWINDOWED + 4u) && b.high_word == 0xdeadbeefu);
    assert(giulia_kiwi_reg_read(&io, 0x00a00004u, &val) == GIULIA_KIWI_OK && val == 0xdeadbeefu);

    /* Bounds and window readback failure */
    assert(giulia_kiwi_reg_read(&io, 0x101u, &val) == GIULIA_KIWI_BOUNDS);
    assert(giulia_kiwi_reg_read(&io, GIULIA_KIWI_MAX_SOC_ADDR, &val) == GIULIA_KIWI_BOUNDS);
    b.bad_window_rb = 1;
    assert(giulia_kiwi_reg_read(&io, 0x00a00004u, &val) == GIULIA_KIWI_READBACK);
    b.bad_window_rb = 0;

    /* SoC identity decoding & firmware name selection */
    b.low_word = 0x41100201u; /* family=4, device=0x110, major=2, minor=1 */
    assert(giulia_kiwi_read_soc_info(&io, &info) == GIULIA_KIWI_OK);
    assert(info.family == 4 && info.device == 0x110 && info.major == 2 && info.minor == 1);
    assert(strcmp(giulia_kiwi_fw_name(&info), "amss20.bin") == 0);

    b.low_word = 0x41100100u; /* major=1 */
    assert(giulia_kiwi_read_soc_info(&io, &info) == GIULIA_KIWI_OK);
    assert(strcmp(giulia_kiwi_fw_name(&info), "amss.bin") == 0);

    b.low_word = 0xffffffffu;
    assert(giulia_kiwi_read_soc_info(&io, &info) == GIULIA_KIWI_READBACK);

    puts("KIWI BAR0 windowed reg & SoC info tests passed");
    return 0;
}
