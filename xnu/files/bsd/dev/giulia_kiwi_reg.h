/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_KIWI_REG_H
#define GIULIA_KIWI_REG_H
#include <stdint.h>

#define GIULIA_KIWI_MAX_UNWINDOWED 0x80000u
#define GIULIA_KIWI_WINDOW_REG     0x310cu
#define GIULIA_KIWI_WINDOW_ENABLE  0x40000000u
#define GIULIA_KIWI_WINDOW_SHIFT   19u
#define GIULIA_KIWI_WINDOW_MASK    0x3fu
#define GIULIA_KIWI_WINDOW_OFFSET  0x7ffffu
#define GIULIA_KIWI_SOC_INFO_REG   0x224u
#define GIULIA_KIWI_MAX_SOC_ADDR   0x2000000u

enum giulia_kiwi_result {
    GIULIA_KIWI_OK = 0,
    GIULIA_KIWI_ARGUMENT = -1,
    GIULIA_KIWI_BOUNDS = -2,
    GIULIA_KIWI_READBACK = -3,
    GIULIA_KIWI_IO = -4
};

struct giulia_kiwi_bar_io {
    void *cookie;
    int (*read32)(void *, uint32_t bar_offset, uint32_t *value);
    int (*write32)(void *, uint32_t bar_offset, uint32_t value);
    uint32_t bar_bytes;
};

struct giulia_kiwi_soc_info {
    uint32_t raw;
    uint8_t family;
    uint16_t device;
    uint8_t major;
    uint8_t minor;
};

/* Windowed BAR0 register access for WCN7850 / KIWI. Caller serializes BAR0. */
int giulia_kiwi_reg_read(const struct giulia_kiwi_bar_io *io, uint32_t soc_addr,
    uint32_t *value);
int giulia_kiwi_reg_write(const struct giulia_kiwi_bar_io *io, uint32_t soc_addr,
    uint32_t value);

/* Read BAR0 + 0x224 SoC identity and pick amss20.bin (major==2) vs amss.bin. */
int giulia_kiwi_read_soc_info(const struct giulia_kiwi_bar_io *io,
    struct giulia_kiwi_soc_info *out);
const char *giulia_kiwi_fw_name(const struct giulia_kiwi_soc_info *info);

#endif
