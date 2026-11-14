/* SPDX-License-Identifier: BSD-2-Clause */
/* Execute the actual controller source against mock DBI/config transactions. */
#include "giulia_pcie_msi.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define ADDR 0x820u
#define ENABLE 0x828u
#define MASK 0x82cu
#define STATUS 0x830u
#define CAP 0x50u
enum { DBI_WRITE = 1, CFG16_WRITE, CFG32_WRITE, BARRIER };
struct event { unsigned kind; uint32_t offset, value; };
struct mock {
    uint32_t dbi[0x1000 / 4];
    uint8_t cfg[0x100];
    struct event events[512];
    unsigned count, reads, delivered;
    uint32_t delivered_bits, ignore_dbi_write, ignore_cfg32_write;
    int refuse_disable, refuse_enable, inject_once;
};
static uint16_t get16(struct mock *p, uint32_t off)
{
    assert(off + 2u <= sizeof(p->cfg) && !(off & 1u));
    return (uint16_t)(p->cfg[off] | ((uint16_t)p->cfg[off + 1] << 8));
}
static uint32_t get32(struct mock *p, uint32_t off)
{
    assert(!(off & 3u));
    return (uint32_t)get16(p, off) | ((uint32_t)get16(p, off + 2) << 16);
}
static void put16(struct mock *p, uint32_t off, uint16_t value)
{
    assert(off + 2u <= sizeof(p->cfg));
    p->cfg[off] = (uint8_t)value;
    p->cfg[off + 1] = (uint8_t)(value >> 8);
}
static void put32(struct mock *p, uint32_t off, uint32_t value)
{
    put16(p, off, (uint16_t)value);
    put16(p, off + 2, (uint16_t)(value >> 16));
}
static void event(struct mock *p, unsigned kind, uint32_t off, uint32_t value)
{
    assert(p->count < sizeof(p->events) / sizeof(p->events[0]));
    p->events[p->count++] = (struct event){kind, off, value};
}
static uint32_t dbi_read(void *cookie, uint32_t off)
{
    struct mock *p = cookie;
    assert(off < sizeof(p->dbi) && !(off & 3u));
    ++p->reads;
    return p->dbi[off / 4];
}
static void dbi_write(void *cookie, uint32_t off, uint32_t value)
{
    struct mock *p = cookie;
    assert(off < sizeof(p->dbi) && !(off & 3u));
    event(p, DBI_WRITE, off, value);
    if (off == p->ignore_dbi_write) return;
    if (off == STATUS) p->dbi[off / 4] &= ~value;
    else p->dbi[off / 4] = value;
}
static uint16_t cfg16_read(void *cookie, uint32_t off)
{
    struct mock *p = cookie;
    ++p->reads;
    return get16(p, off);
}
static uint32_t cfg32_read(void *cookie, uint32_t off)
{
    struct mock *p = cookie;
    ++p->reads;
    return get32(p, off);
}
static void cfg16_write(void *cookie, uint32_t off, uint16_t value)
{
    struct mock *p = cookie;
    event(p, CFG16_WRITE, off, value);
    if (off == CAP + 2u && ((p->refuse_disable && !(value & 1u)) ||
        (p->refuse_enable && (value & 1u)))) return;
    put16(p, off, value);
}
static void cfg32_write(void *cookie, uint32_t off, uint32_t value)
{
    struct mock *p = cookie;
    event(p, CFG32_WRITE, off, value);
    if (off == p->ignore_cfg32_write) return;
    put32(p, off, value);
}
static void barrier(void *cookie) { event(cookie, BARRIER, 0, 0); }
static struct giulia_pcie_msi_io io_for(struct mock *p)
{
    return (struct giulia_pcie_msi_io){p, dbi_read, dbi_write, cfg16_read,
        cfg16_write, cfg32_read, cfg32_write, barrier};
}
static struct giulia_pcie_msi_config config(void)
{
    /* Reserved synthetic CFG0 bus target; deliberately not Android's IOVA. */
    return (struct giulia_pcie_msi_config){0x60100000u, GIULIA_MSI_REQUIRED,
        CAP, 32};
}
static void reset(struct mock *p, struct giulia_pcie_msi *m)
{
    memset(p, 0, sizeof(*p));
    memset(m, 0, sizeof(*m));
    p->ignore_dbi_write = UINT32_MAX;
    p->ignore_cfg32_write = UINT32_MAX;
    put16(p, CAP, 5);
    put16(p, CAP + 2u, 0x015au); /* MMC=32, MME=32, PVM, MSI disabled. */
}
static void deliver(void *cookie, unsigned vector)
{
    struct mock *p = cookie;
    ++p->delivered;
    p->delivered_bits |= 1u << vector;
    if (p->inject_once) {
        /* Must survive the previous event's acknowledgement. */
        p->dbi[STATUS / 4] |= 1u;
        p->inject_once = 0;
    }
}
static void admission(void)
{
    struct mock p;
    struct giulia_pcie_msi m;
    struct giulia_pcie_msi_config c;
    struct giulia_pcie_msi_io io = io_for(&p);
    unsigned i;
    const uint64_t bad_targets[] = {0, 3, 0x100000000ULL, 0x160100000ULL,
        0x17110040, 0x17150040, 0xffffffff};
    reset(&p, &m); m.state = 99;
    assert(giulia_pcie_msi_stop(&m) == GIULIA_MSI_STATE);
    assert(!p.reads && !p.count);
    for (i = 0; i < 4; ++i) {
        reset(&p, &m); c = config(); c.prerequisites &= ~(1u << i);
        assert(giulia_pcie_msi_prepare(&m, &c, &io) == GIULIA_MSI_PREREQUISITE);
        assert(!p.reads && !p.count);
    }
    for (i = 0; i < sizeof(bad_targets) / sizeof(bad_targets[0]); ++i) {
        reset(&p, &m); c = config(); c.target = bad_targets[i];
        assert(giulia_pcie_msi_prepare(&m, &c, &io) == GIULIA_MSI_ARGUMENT);
        assert(!p.reads && !p.count);
    }
    for (i = 0; i < 8; ++i) {
        reset(&p, &m); c = config(); p.dbi[(ENABLE + 12u * i) / 4] = 1;
        assert(giulia_pcie_msi_prepare(&m, &c, &io) == GIULIA_MSI_RECEIVER_BUSY);
        assert(!p.count && m.state == GIULIA_MSI_IDLE);
    }
    reset(&p, &m); c = config(); put16(&p, CAP + 2u, 0x15b);
    assert(giulia_pcie_msi_prepare(&m, &c, &io) == GIULIA_MSI_SOURCE_ENABLED);
    assert(!p.count);
    reset(&p, &m); c = config(); put16(&p, CAP, 0x11); /* MSI-X */
    assert(giulia_pcie_msi_prepare(&m, &c, &io) == GIULIA_MSI_CAPABILITY);
    assert(!p.count);
    reset(&p, &m); c = config(); put16(&p, CAP + 2u, 0x10a); c.vectors = 3;
    assert(giulia_pcie_msi_prepare(&m, &c, &io) == GIULIA_MSI_ARGUMENT);
    assert(!p.count && !p.reads);
    reset(&p, &m); c = config(); put16(&p, CAP + 2u, 0x104); /* MMC=4 */
    assert(giulia_pcie_msi_prepare(&m, &c, &io) == GIULIA_MSI_CAPABILITY);
    assert(!p.count);
    reset(&p, &m); c = config(); put16(&p, CAP + 2u, 0x114); /* MME=2 <=4 */
    c.vectors = 4;
    assert(!giulia_pcie_msi_prepare(&m, &c, &io));
    assert((get16(&p, CAP + 2u) & 0x70u) == 0x20u);
    assert(!giulia_pcie_msi_stop(&m));
    reset(&p, &m); c = config(); c.vectors = 16;
    assert(!giulia_pcie_msi_prepare(&m, &c, &io));
    i = p.count;
    assert(giulia_pcie_msi_arm(&m, 173, 1u << 16) == GIULIA_MSI_ARGUMENT);
    assert(p.count == i);
    assert(!giulia_pcie_msi_arm(&m, 173, 1));
    i = p.count;
    assert(giulia_pcie_msi_mask(&m, 1u << 16) == GIULIA_MSI_ARGUMENT);
    assert(giulia_pcie_msi_unmask(&m, 1u << 16) == GIULIA_MSI_ARGUMENT);
    assert(p.count == i);
    assert(!giulia_pcie_msi_stop(&m));
    reset(&p, &m); c = config(); put16(&p, CAP + 2u, 0x154); /* MME > MMC */
    assert(giulia_pcie_msi_prepare(&m, &c, &io) == GIULIA_MSI_CAPABILITY);
    assert(!p.count);
    reset(&p, &m); c = config(); c.capability = 0xf0;
    put16(&p, 0xf0, 5); put16(&p, 0xf2, 0x18a); /* Mask/pending beyond config */
    assert(giulia_pcie_msi_prepare(&m, &c, &io) == GIULIA_MSI_CAPABILITY);
    assert(!p.count);
}
static void formats_and_counts(void)
{
    struct mock p;
    struct giulia_pcie_msi m;
    struct giulia_pcie_msi_config c;
    struct giulia_pcie_msi_io io = io_for(&p);
    unsigned n, wide, pvm;
    for (n = 1; n <= 32; n *= 2) for (wide = 0; wide < 2; ++wide)
        for (pvm = 0; pvm < 2; ++pvm) {
            uint32_t mask = n == 32 ? UINT32_MAX : (1u << n) - 1u;
            uint32_t data = CAP + (wide ? 12u : 8u);
            reset(&p, &m); c = config(); c.vectors = (uint8_t)n;
            put16(&p, CAP + 2, 0xau | (wide ? 0x80u : 0u) | (pvm ? 0x100u : 0u));
            assert(!giulia_pcie_msi_prepare(&m, &c, &io));
            assert(m.state == GIULIA_MSI_PREPARED && m.valid_mask == mask);
            assert(p.dbi[ENABLE / 4] == mask && p.dbi[MASK / 4] == UINT32_MAX);
            assert(p.dbi[ADDR / 4] == c.target && !p.dbi[(ADDR + 4) / 4]);
            assert(get32(&p, CAP + 4) == c.target && !get16(&p, data));
            if (wide) assert(!get32(&p, CAP + 8));
            assert(!giulia_pcie_msi_arm(&m, 173, mask));
            assert(get16(&p, CAP + 2) & 1u);
            if (pvm) assert(!(get32(&p, m.mask_offset) & mask));
            assert(!giulia_pcie_msi_stop(&m));
        }
}
static void pending_and_exit(void)
{
    struct mock p;
    struct giulia_pcie_msi m;
    struct giulia_pcie_msi_config c = config();
    struct giulia_pcie_msi_io io = io_for(&p);
    unsigned before;
    reset(&p, &m);
    assert(!giulia_pcie_msi_prepare(&m, &c, &io));
    before = p.count;
    assert(giulia_pcie_msi_arm(&m, 141, 1) == GIULIA_MSI_ARGUMENT);
    assert(p.count == before); /* DT SPI offset is not the GIC INTID. */
    assert(!giulia_pcie_msi_arm(&m, 173, 0x80000001u));
    p.dbi[STATUS / 4] = 0x80000003u; p.inject_once = 1;
    assert(giulia_pcie_msi_dispatch(&m, deliver, &p) == 0x80000001u);
    assert(p.delivered == 2 && p.delivered_bits == 0x80000001u);
    assert(p.dbi[STATUS / 4] == 3u); /* masked bit1 and new bit0 survive */
    assert(giulia_pcie_msi_dispatch(&m, deliver, &p) == 1u);
    assert(p.dbi[STATUS / 4] == 2u);
    assert(!giulia_pcie_msi_unmask(&m, 2));
    assert(giulia_pcie_msi_dispatch(&m, deliver, &p) == 2u);
    assert(!p.dbi[STATUS / 4]);
    assert(!giulia_pcie_msi_mask(&m, 1));
    assert((p.dbi[MASK / 4] & 1u) && (get32(&p, m.mask_offset) & 1u));
    before = p.count;
    assert(!giulia_pcie_msi_stop(&m));
    assert(p.events[before].kind == CFG16_WRITE &&
        p.events[before].offset == CAP + 2u && !(p.events[before].value & 1u));
    assert(m.state == GIULIA_MSI_STOPPED && !p.dbi[ENABLE / 4]);
    before = p.count;
    assert(!giulia_pcie_msi_stop(&m));
    assert(!giulia_pcie_msi_dispatch(&m, deliver, &p));
    assert(giulia_pcie_msi_unmask(&m, 1) == GIULIA_MSI_STATE);
    assert(p.count == before);
}
static void failures(void)
{
    struct mock p;
    struct giulia_pcie_msi m;
    struct giulia_pcie_msi_config c = config();
    struct giulia_pcie_msi_io io = io_for(&p);
    reset(&p, &m); p.ignore_dbi_write = ADDR;
    assert(giulia_pcie_msi_prepare(&m, &c, &io) == GIULIA_MSI_READBACK);
    assert(m.state == GIULIA_MSI_STOPPED && !(get16(&p, CAP + 2) & 1u));
    reset(&p, &m);
    assert(!giulia_pcie_msi_prepare(&m, &c, &io)); p.refuse_enable = 1;
    assert(giulia_pcie_msi_arm(&m, 173, 1) == GIULIA_MSI_READBACK);
    assert(m.state == GIULIA_MSI_STOPPED && !p.dbi[ENABLE / 4]);
    reset(&p, &m);
    assert(!giulia_pcie_msi_prepare(&m, &c, &io));
    assert(!giulia_pcie_msi_arm(&m, 173, 1)); p.refuse_disable = 1;
    assert(giulia_pcie_msi_stop(&m) == GIULIA_MSI_READBACK);
    assert(m.state == GIULIA_MSI_FAULT && p.dbi[ENABLE / 4] == UINT32_MAX);
    assert(p.dbi[MASK / 4] == UINT32_MAX && (get16(&p, CAP + 2) & 1u));
    p.refuse_disable = 0;
    assert(!giulia_pcie_msi_stop(&m) && m.state == GIULIA_MSI_STOPPED);
    reset(&p, &m);
    assert(!giulia_pcie_msi_prepare(&m, &c, &io));
    assert(!giulia_pcie_msi_arm(&m, 173, 1)); p.ignore_dbi_write = ENABLE;
    assert(giulia_pcie_msi_stop(&m) == GIULIA_MSI_READBACK);
    assert(m.state == GIULIA_MSI_FAULT && !(get16(&p, CAP + 2) & 1u));
    p.ignore_dbi_write = UINT32_MAX;
    assert(!giulia_pcie_msi_stop(&m));
    reset(&p, &m); p.ignore_cfg32_write = CAP + 12u;
    assert(giulia_pcie_msi_prepare(&m, &c, &io) == GIULIA_MSI_READBACK);
    assert(m.state == GIULIA_MSI_STOPPED);
    reset(&p, &m);
    assert(!giulia_pcie_msi_prepare(&m, &c, &io));
    p.ignore_cfg32_write = CAP + 12u;
    assert(giulia_pcie_msi_arm(&m, 173, 1) == GIULIA_MSI_READBACK);
    assert(m.state == GIULIA_MSI_STOPPED && !(get16(&p, CAP + 2) & 1u));
    reset(&p, &m);
    assert(!giulia_pcie_msi_prepare(&m, &c, &io));
    assert(!giulia_pcie_msi_arm(&m, 173, 1));
    p.ignore_cfg32_write = CAP + 12u;
    assert(giulia_pcie_msi_mask(&m, 1) == GIULIA_MSI_READBACK);
    assert(m.state == GIULIA_MSI_STOPPED);
    reset(&p, &m);
    assert(!giulia_pcie_msi_prepare(&m, &c, &io));
    assert(!giulia_pcie_msi_arm(&m, 173, 1));
    p.ignore_cfg32_write = CAP + 12u;
    assert(giulia_pcie_msi_unmask(&m, 2) == GIULIA_MSI_READBACK);
    assert(m.state == GIULIA_MSI_STOPPED);
}
int main(void)
{
    admission(); formats_and_counts(); pending_and_exit(); failures();
    puts("SM8650 SNPS MSI mock-MMIO tests passed");
    return 0;
}
