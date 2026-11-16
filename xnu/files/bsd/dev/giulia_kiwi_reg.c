/* SPDX-License-Identifier: BSD-2-Clause */
#include "giulia_kiwi_reg.h"

/* Windowed WCN7850/KIWI BAR0 access & SoC identity decoding based on
 * cnss2 pci.c (__cnss_get_mhi_soc_info / cnss_pci_reg_read / fw selection). */
static int valid_io(const struct giulia_kiwi_bar_io *io, int need_write)
{
    if (!io || !io->read32 || (need_write && !io->write32)) return 0;
    if (io->bar_bytes < GIULIA_KIWI_MAX_UNWINDOWED + 0x80000u) return 0;
    return 1;
}

static int select_window(const struct giulia_kiwi_bar_io *io, uint32_t soc_addr,
    uint32_t *bar_offset)
{
    if ((soc_addr & 3u) || soc_addr >= GIULIA_KIWI_MAX_SOC_ADDR)
        return GIULIA_KIWI_BOUNDS;
    if (soc_addr < GIULIA_KIWI_MAX_UNWINDOWED) {
        *bar_offset = soc_addr;
        return GIULIA_KIWI_OK;
    }
    if (!io->write32) return GIULIA_KIWI_ARGUMENT;
    uint32_t win = GIULIA_KIWI_WINDOW_ENABLE |
        ((soc_addr >> GIULIA_KIWI_WINDOW_SHIFT) & GIULIA_KIWI_WINDOW_MASK);
    uint32_t rb = 0;
    if (io->write32(io->cookie, GIULIA_KIWI_WINDOW_REG, win))
        return GIULIA_KIWI_IO;
    if (io->read32(io->cookie, GIULIA_KIWI_WINDOW_REG, &rb))
        return GIULIA_KIWI_IO;
    if (rb != win) return GIULIA_KIWI_READBACK;
    *bar_offset = GIULIA_KIWI_MAX_UNWINDOWED + (soc_addr & GIULIA_KIWI_WINDOW_OFFSET);
    return GIULIA_KIWI_OK;
}

int giulia_kiwi_reg_read(const struct giulia_kiwi_bar_io *io, uint32_t soc_addr,
    uint32_t *value)
{
    uint32_t off = 0;
    int rc;
    if (!value || !valid_io(io, soc_addr >= GIULIA_KIWI_MAX_UNWINDOWED))
        return GIULIA_KIWI_ARGUMENT;
    rc = select_window(io, soc_addr, &off);
    if (rc) return rc;
    return io->read32(io->cookie, off, value) ? GIULIA_KIWI_IO : GIULIA_KIWI_OK;
}

int giulia_kiwi_reg_write(const struct giulia_kiwi_bar_io *io, uint32_t soc_addr,
    uint32_t value)
{
    uint32_t off = 0;
    int rc;
    if (!valid_io(io, 1)) return GIULIA_KIWI_ARGUMENT;
    rc = select_window(io, soc_addr, &off);
    if (rc) return rc;
    return io->write32(io->cookie, off, value) ? GIULIA_KIWI_IO : GIULIA_KIWI_OK;
}

int giulia_kiwi_read_soc_info(const struct giulia_kiwi_bar_io *io,
    struct giulia_kiwi_soc_info *out)
{
    uint32_t raw = 0;
    int rc;
    if (!out || !valid_io(io, 0)) return GIULIA_KIWI_ARGUMENT;
    rc = giulia_kiwi_reg_read(io, GIULIA_KIWI_SOC_INFO_REG, &raw);
    if (rc) return rc;
    if (raw == 0xffffffffu || raw == 0u) return GIULIA_KIWI_READBACK;
    out->raw = raw;
    out->family = (uint8_t)((raw >> 28) & 0xfu);
    out->device = (uint16_t)((raw >> 16) & 0xfffu);
    out->major = (uint8_t)((raw >> 8) & 0xffu);
    out->minor = (uint8_t)(raw & 0xffu);
    return GIULIA_KIWI_OK;
}

const char *giulia_kiwi_fw_name(const struct giulia_kiwi_soc_info *info)
{
    if (!info) return 0;
    return info->major == 2u ? "amss20.bin" : "amss.bin";
}
