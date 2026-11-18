/* SPDX-License-Identifier: BSD-2-Clause */
/* Cloud-only host fixture for the real PCIe link core. No hardware/SDK. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "giulia_pcie_link.h"

#define BIT(n) (1u << (n))
struct fake {
    uint32_t regs[4][0x3000 / 4];
    uint32_t original[4][0x3000 / 4];
    unsigned reads, writes, cfg_accesses, bar_probes, barriers;
    unsigned core_reads, core_writes, cold_aperture, allow_memory, perst_asserted, core_dark;
    char memory_events[64];
    unsigned memory_event_count;
    unsigned link_reads, link_after, crs_reads, drop_link_after;
    uint16_t negotiated_profile;
    unsigned profile_drift_after_cfg;
    uint64_t delay_total;
    uint32_t bar_mask;
    unsigned never_link, lost_link, stuck_link, check_decoders, old_decoder;
    unsigned drop_region;
    uint32_t drop_offset, drop_mask, drop_value;
    unsigned drop_on, drop_value_only;
    unsigned fault_on, fault_region, fault_after_writes;
    uint32_t fault_offset, fault_required_bits;
};

static uint16_t raw16(struct fake *f, unsigned r, uint32_t o)
{ return (uint16_t)(f->regs[r][o / 4] >> ((o & 2u) * 8u)); }
static void seed16(struct fake *f, unsigned r, uint32_t o, uint16_t v)
{
    uint32_t shift = (o & 2u) * 8u;
    f->regs[r][o / 4] = (f->regs[r][o / 4] & ~(0xffffu << shift)) |
        ((uint32_t)v << shift);
}
static int up(struct fake *f)
{
    if (f->stuck_link) return 1;
    return (f->regs[GIULIA_LINK_PARF][0x1b0 / 4] & BIT(8)) &&
        !f->never_link && !f->lost_link && f->link_reads >= f->link_after;
}
/* Model the candidate downstream aperture separately from its setup code.
 * The CPU view starts at 0x60000000; PARF uses offsets within that aperture.
 * A mismatched decoder produces an unmapped read, rather than magically
 * exposing the fake register array. This model is not silicon evidence.
 */
static int aperture_decodes(struct fake *f, unsigned r, uint32_t o, unsigned width)
{
    uint64_t size, decoder, local;
    unsigned base;
    if (!f->cold_aperture || r == GIULIA_LINK_PARF) return 1;
    size = ((uint64_t)f->regs[GIULIA_LINK_PARF][0x35c / 4] << 32) |
        f->regs[GIULIA_LINK_PARF][0x358 / 4];
    if (r == GIULIA_LINK_CFG) return 0x00100000ull + o + width <= size;
    base = r == GIULIA_LINK_DBI ? 0x350u : 0x634u;
    decoder = ((uint64_t)f->regs[GIULIA_LINK_PARF][(base + 4u) / 4] << 32) |
        f->regs[GIULIA_LINK_PARF][base / 4];
    local = (r == GIULIA_LINK_DBI ? 0ull : 0x1000ull) + o;
    return decoder + o == local && local + width <= size;
}
static void access_check(struct fake *f, unsigned r, uint32_t o, unsigned width)
{
    assert(r < 4 && o + width <= (r == GIULIA_LINK_PARF ? 0x3000u : 0x1000u));
    assert(!(o & (width - 1u)));
    if (f->core_dark) assert(r == GIULIA_LINK_PARF);
    if (f->check_decoders && f->old_decoder)
        assert(r != GIULIA_LINK_DBI && r != GIULIA_LINK_ATU);
    if (f->cold_aperture && r != GIULIA_LINK_PARF) {
        /* Cold RC reset leaves DBI/ATU inaccessible until PARF setup. */
        assert(f->regs[GIULIA_LINK_PARF][0x1000 / 4] == 4);
        assert(!(f->regs[GIULIA_LINK_PARF][0x40 / 4] & 1));
        assert(f->regs[GIULIA_LINK_PARF][0x358 / 4] == 0x04000000u);
        assert(f->regs[GIULIA_LINK_PARF][0x35c / 4] == 0);
        assert(!(f->regs[GIULIA_LINK_PARF][0] & BIT(29)));
        assert(f->regs[GIULIA_LINK_PARF][0x174 / 4] & BIT(4));
        assert(!(f->regs[GIULIA_LINK_PARF][0x20 / 4] & BIT(5)));
        assert(f->regs[GIULIA_LINK_PARF][0x1a8 / 4] & BIT(31));
    }
    if (r == GIULIA_LINK_CFG) {
        assert(!f->perst_asserted);
        assert(up(f));
        assert(f->regs[GIULIA_LINK_ATU][4 / 4] & BIT(31));
        f->cfg_accesses++;
    }
}
static void after_cfg(struct fake *f, unsigned r)
{
    if (r == GIULIA_LINK_CFG && f->drop_link_after &&
        f->cfg_accesses == f->drop_link_after) f->lost_link = 1;
    if (r == GIULIA_LINK_CFG && f->profile_drift_after_cfg &&
        f->cfg_accesses == f->profile_drift_after_cfg) f->negotiated_profile = 0x12;
}
static int fault_read(struct fake *f, unsigned r, uint32_t o)
{
    return f->fault_on && f->fault_region == r && f->fault_offset == o &&
        f->writes >= f->fault_after_writes &&
        (f->regs[r][o / 4] & f->fault_required_bits) == f->fault_required_bits;
}
static uint32_t read32(void *p, unsigned r, uint32_t o)
{
    struct fake *f = p;
    uint32_t v;
    access_check(f, r, o, 4);
    f->reads++;
    if (r != GIULIA_LINK_PARF) f->core_reads++;
    if (!aperture_decodes(f, r, o, 4)) return 0xffffffffu;
    v = f->regs[r][o / 4];
    if (r == GIULIA_LINK_CFG && o == 0 && f->crs_reads) {
        f->crs_reads--;
        v = 0xffff0001u;
    }
    if (r == GIULIA_LINK_CFG && o == 0x10 && v == 0xffffffffu)
        v = f->bar_mask;
    if (fault_read(f, r, o)) v = 0xffffffffu;
    after_cfg(f, r);
    return v;
}
static uint16_t read16(void *p, unsigned r, uint32_t o)
{
    struct fake *f = p;
    uint16_t v;
    access_check(f, r, o, 2);
    f->reads++;
    if (r != GIULIA_LINK_PARF) f->core_reads++;
    if (!aperture_decodes(f, r, o, 2)) return 0xffffu;
    v = raw16(f, r, o);
    if (r == GIULIA_LINK_DBI && o == 0x82) {
        if (f->regs[GIULIA_LINK_PARF][0x1b0 / 4] & BIT(8)) f->link_reads++;
        v = up(f) ? (uint16_t)(BIT(13) | f->negotiated_profile) : 0;
    }
    if (fault_read(f, r, o)) v = 0xffffu;
    after_cfg(f, r);
    return v;
}
static uint32_t dropped(struct fake *f, unsigned r, uint32_t o,
    uint32_t value, uint32_t old)
{
    if (f->drop_on && r == f->drop_region && o == f->drop_offset &&
        (!f->drop_value_only || value == f->drop_value))
        return (value & ~f->drop_mask) | (old & f->drop_mask);
    return value;
}
static void write32(void *p, unsigned r, uint32_t o, uint32_t v)
{
    struct fake *f = p;
    access_check(f, r, o, 4);
    f->writes++;
    assert(aperture_decodes(f, r, o, 4));
    if (r != GIULIA_LINK_PARF) f->core_writes++;
    /* A 32-bit command/status write would acknowledge W1C status bits. */
    assert(!((r == GIULIA_LINK_DBI || r == GIULIA_LINK_CFG) && o == 4));
    if (r == GIULIA_LINK_CFG && (o == 0x10 || o == 0x14)) {
        assert((raw16(f, GIULIA_LINK_CFG, 4) & 7u) == 0);
        assert((raw16(f, GIULIA_LINK_CFG, 0x52) & 1u) == 0);
        if (o == 0x10 && v == 0xffffffffu) f->bar_probes++;
    }
    v = dropped(f, r, o, v, f->regs[r][o / 4]);
    f->regs[r][o / 4] = v;
    if (r == GIULIA_LINK_DBI && o == 0x80c) f->regs[r][o / 4] &= ~BIT(17);
    if (r == GIULIA_LINK_PARF && o == 0x350 && f->check_decoders)
        f->old_decoder = v != 0;
    after_cfg(f, r);
}
static void write16(void *p, unsigned r, uint32_t o, uint16_t v)
{
    struct fake *f = p;
    access_check(f, r, o, 2);
    f->writes++;
    assert(aperture_decodes(f, r, o, 2));
    if (r != GIULIA_LINK_PARF) f->core_writes++;
    if ((r == GIULIA_LINK_DBI || r == GIULIA_LINK_CFG) && o == 4) {
        assert((v & 5u) == 0); /* No IO or BME, including failures. */
        if (v & BIT(1)) assert(f->allow_memory);
        if (f->allow_memory) {
            assert(f->memory_event_count < sizeof(f->memory_events) - 1);
            f->memory_events[f->memory_event_count++] = r == GIULIA_LINK_DBI ?
                ((v & BIT(1)) ? 'R' : 'r') : ((v & BIT(1)) ? 'E' : 'e');
        }
    }
    v = (uint16_t)dropped(f, r, o, v, raw16(f, r, o));
    seed16(f, r, o, v);
    after_cfg(f, r);
}
static void barrier(void *p) { ((struct fake *)p)->barriers++; }
static void delay_us(void *p, uint32_t us)
{ assert(us <= 9000); ((struct fake *)p)->delay_total += us; }

static struct giulia_pcie_link_io io_for(struct fake *f)
{
    return (struct giulia_pcie_link_io){f, read32, write32, read16, write16,
        barrier, delay_us};
}
static void init(struct fake *f, struct giulia_pcie_link *s)
{
    memset(f, 0, sizeof(*f));
    memset(s, 0, sizeof(*s));
    f->cold_aperture = 1;
    f->bar_mask = 0xffe00004u;
    f->link_after = 3;
    f->negotiated_profile = 0x11;
    f->regs[GIULIA_LINK_PARF][0] = BIT(29) | 0x365eu;
    f->regs[GIULIA_LINK_PARF][0x40 / 4] = 1;
    f->regs[GIULIA_LINK_PARF][0x20 / 4] = BIT(5);
    /* Deliberately inherit a different decoder so restore/retry tests detect
     * any access after returning to the previous address convention. */
    f->regs[GIULIA_LINK_PARF][0x350 / 4] = 0x60000000u;
    f->regs[GIULIA_LINK_PARF][0x634 / 4] = 0x60001000u;
    f->regs[GIULIA_LINK_DBI][0] = 0x011317cbu;
    seed16(f, GIULIA_LINK_DBI, 4, BIT(10));
    seed16(f, GIULIA_LINK_DBI, 6, 0xa510u);
    seed16(f, GIULIA_LINK_DBI, 0xe, 1);
    seed16(f, GIULIA_LINK_DBI, 0x34, 0x70);
    seed16(f, GIULIA_LINK_DBI, 0x70, 0x10);
    seed16(f, GIULIA_LINK_DBI, 0x72, 0x42); /* v2 root port */
    seed16(f, GIULIA_LINK_DBI, 0xa0, 0x23); /* inherited Gen3 target */
    seed16(f, GIULIA_LINK_DBI, 0xa2, 0x8090); /* status must not be written */
    f->regs[GIULIA_LINK_DBI][0x7c / 4] = 0x22c43u;
    f->regs[GIULIA_LINK_DBI][0x84 / 4] = BIT(6);
    f->regs[GIULIA_LINK_DBI][0x8f8 / 4] = 0x3530302au;
    f->regs[GIULIA_LINK_CFG][0] = GIULIA_PCIE_LINK_EP_ID;
    seed16(f, GIULIA_LINK_CFG, 4, BIT(10));
    seed16(f, GIULIA_LINK_CFG, 6, 0x4310u);
    seed16(f, GIULIA_LINK_CFG, 0x34, 0x40);
    seed16(f, GIULIA_LINK_CFG, 0x40, 0x5001);
    seed16(f, GIULIA_LINK_CFG, 0x50, 0x7005);
    seed16(f, GIULIA_LINK_CFG, 0x52, 0x015b); /* Must be disabled by probe. */
    seed16(f, GIULIA_LINK_CFG, 0x70, 0x10);
    f->regs[GIULIA_LINK_CFG][0x10 / 4] = 4;
    memcpy(f->original, f->regs, sizeof(f->regs));
}
static int prepare_fixture(struct giulia_pcie_link *s, uint32_t flags,
    const struct giulia_pcie_link_io *io)
{
    int result = giulia_pcie_link_prepare_parf(s, flags, io);
    if (result) return result;
    result = giulia_pcie_link_prepare(s, flags | GIULIA_LINK_PHY_READY);
    if (result && s->parf_owned && !s->core_owned) {
        int cleanup = giulia_pcie_link_stop(s, GIULIA_LINK_STOP_REQUIRED);
        if (cleanup) return cleanup;
    }
    return result;
}

static int quiesce_then_stop(struct fake *f, struct giulia_pcie_link *s)
{
    int result;
    f->perst_asserted = 1;
    result = giulia_pcie_link_quiesce(s, GIULIA_LINK_QUIESCE_REQUIRED |
        (f->core_dark ? GIULIA_LINK_PHY_QUIET : GIULIA_LINK_PHY_READY));
    if (result) return result;
    f->core_dark = 1;
    return giulia_pcie_link_stop(s, GIULIA_LINK_STOP_REQUIRED);
}

static void prepare(struct fake *f, struct giulia_pcie_link *s)
{
    struct giulia_pcie_link_io io = io_for(f);
    assert(prepare_fixture(s, GIULIA_LINK_PARF_REQUIRED, &io) == 0);
    assert(s->state == GIULIA_LINK_PREPARED && s->saved_count <= 48);
    assert((f->regs[GIULIA_LINK_PARF][0x1b0 / 4] & BIT(8)) == 0);
    assert((raw16(f, GIULIA_LINK_DBI, 4) & 7u) == 0);
    assert((raw16(f, GIULIA_LINK_DBI, 0xa0) & 0xfu) == 1);
    assert(raw16(f, GIULIA_LINK_DBI, 0xa2) == 0x8090);
    assert((f->regs[GIULIA_LINK_DBI][0x7c / 4] & 0x3ffu) == 0x11);
    assert(((f->regs[GIULIA_LINK_DBI][0x710 / 4] >> 16) & 0x3fu) == 1);
    assert(((f->regs[GIULIA_LINK_DBI][0x80c / 4] >> 8) & 0x1fu) == 1);
}
static void stop(struct fake *f, struct giulia_pcie_link *s)
{
    unsigned accesses = f->cfg_accesses;
    f->check_decoders = 1;
    f->perst_asserted = 1;
    assert(quiesce_then_stop(f, s) == 0);
    assert(s->state == GIULIA_LINK_STOPPED);
    assert(f->cfg_accesses == accesses); /* No config accesses after PERST#. */
    assert(memcmp(f->regs[GIULIA_LINK_PARF], f->original[GIULIA_LINK_PARF],
        sizeof(f->regs[GIULIA_LINK_PARF])) == 0);
    assert(memcmp(f->regs[GIULIA_LINK_DBI], f->original[GIULIA_LINK_DBI],
        sizeof(f->regs[GIULIA_LINK_DBI])) == 0);
    assert(memcmp(f->regs[GIULIA_LINK_ATU], f->original[GIULIA_LINK_ATU],
        sizeof(f->regs[GIULIA_LINK_ATU])) == 0);
}

static void test_downstream_aperture(void)
{
    struct fake f; struct giulia_pcie_link s;
    init(&f, &s);
    f.regs[GIULIA_LINK_PARF][0x358 / 4] = 0x04000000u;
    f.original[GIULIA_LINK_PARF][0x358 / 4] = 0x04000000u;
    assert(!aperture_decodes(&f, GIULIA_LINK_DBI, 0, 4));
    assert(!aperture_decodes(&f, GIULIA_LINK_ATU, 0, 4));
    prepare(&f, &s);
    assert(read32(&f, GIULIA_LINK_DBI, 0) == 0x011317cbu);
    assert(read32(&f, GIULIA_LINK_ATU, 0) == 0);
    /* Reproduce the previous mixed CPU-address/64 MiB profile: the model
     * must reject it rather than return a fake working root complex. */
    f.regs[GIULIA_LINK_PARF][0x350 / 4] = 0x60000000u;
    f.regs[GIULIA_LINK_PARF][0x634 / 4] = 0x60001000u;
    assert(read32(&f, GIULIA_LINK_DBI, 0) == 0xffffffffu);
    assert(read16(&f, GIULIA_LINK_DBI, 0xe) == 0xffffu);
    assert(read32(&f, GIULIA_LINK_ATU, 0) == 0xffffffffu);
    f.regs[GIULIA_LINK_PARF][0x350 / 4] = 0;
    f.regs[GIULIA_LINK_PARF][0x634 / 4] = 0x1000;
    stop(&f, &s);
    puts("Ace5 PCIe local aperture: mixed address profiles rejected and inherited decoder restored");
}
static void test_default_and_gates(void)
{
    struct fake f; struct giulia_pcie_link s; struct giulia_pcie_link_io io;
    unsigned n;
    init(&f, &s); io = io_for(&f);
    assert(giulia_pcie_link_stop(&s, 0) == 0);
    assert(prepare_fixture(&s, 0, &io) == GIULIA_LINK_PREREQUISITE);
    assert(f.reads == 0 && f.writes == 0);
    for (unsigned bit = 0; bit < 4; bit++) {
        assert(prepare_fixture(&s,
            GIULIA_LINK_PARF_REQUIRED & ~BIT(bit), &io) == GIULIA_LINK_PREREQUISITE);
    }
    assert(f.reads == 0 && f.writes == 0);
    prepare(&f, &s); n = f.reads + f.writes;
    assert(giulia_pcie_link_train(&s, GIULIA_LINK_TRAIN_REQUIRED &
        ~GIULIA_LINK_PHY_READY) == GIULIA_LINK_PREREQUISITE);
    assert(giulia_pcie_link_stop(&s, GIULIA_LINK_STOP_REQUIRED &
        ~GIULIA_LINK_PERST_ASSERTED) == GIULIA_LINK_PREREQUISITE);
    assert(prepare_fixture(&s, GIULIA_LINK_PARF_REQUIRED, &io) == GIULIA_LINK_STATE);
    assert(f.reads + f.writes == n);
    stop(&f, &s); n = f.reads + f.writes;
    assert(giulia_pcie_link_stop(&s, 0) == 0 && f.reads + f.writes == n);
}
static void test_success(void)
{
    struct fake f; struct giulia_pcie_link s;
    init(&f, &s); prepare(&f, &s);
    assert(giulia_pcie_link_identify(&s, GIULIA_LINK_TRAIN_REQUIRED) == 0);
    assert(s.state == GIULIA_LINK_IDENTIFIED && s.endpoint_id == GIULIA_PCIE_LINK_EP_ID);
    assert(!f.bar_probes && !s.memory_enabled && f.cfg_accesses == 1);
    assert(!memcmp(f.regs[GIULIA_LINK_CFG], f.original[GIULIA_LINK_CFG], sizeof(f.regs[0])));
    assert(!f.regs[GIULIA_LINK_ATU][0x204 / 4]);
    stop(&f, &s);
    init(&f, &s); prepare(&f, &s);
    f.crs_reads = 3;
    assert(giulia_pcie_link_train(&s, GIULIA_LINK_TRAIN_REQUIRED) == 0);
    assert(s.state == GIULIA_LINK_READY && s.stage == GIULIA_LINK_STAGE_READY);
    assert(s.endpoint_id == GIULIA_PCIE_LINK_EP_ID && s.bar0_mask == f.bar_mask);
    assert(s.root_pcie_cap == 0x70 && s.endpoint_msi_cap == 0x50);
    assert(f.regs[GIULIA_LINK_ATU][0] == 4);
    assert(f.regs[GIULIA_LINK_ATU][0x14 / 4] == 0x01000000u);
    assert(f.regs[GIULIA_LINK_ATU][0x08 / 4] == 0x60100000u);
    assert(f.regs[GIULIA_LINK_ATU][0x10 / 4] == 0x601fffffu);
    assert(f.regs[GIULIA_LINK_ATU][0x200 / 4] == 0);
    assert(f.regs[GIULIA_LINK_ATU][0x214 / 4] == 0x60400000u);
    assert(f.regs[GIULIA_LINK_ATU][0x210 / 4] == 0x605fffffu);
    assert(f.regs[GIULIA_LINK_CFG][0x10 / 4] == 0x60400004u);
    assert(f.regs[GIULIA_LINK_CFG][0x14 / 4] == 0);
    assert((raw16(&f, GIULIA_LINK_CFG, 4) & 7u) == 0);
    assert((raw16(&f, GIULIA_LINK_CFG, 0x52) & 1u) == 0);
    assert(raw16(&f, GIULIA_LINK_DBI, 6) == 0xa510u);
    assert(raw16(&f, GIULIA_LINK_CFG, 6) == 0x4310u);
    assert(f.bar_probes == 1 && f.delay_total >= 3000);
    stop(&f, &s);
}
static void test_busy(void)
{
    struct fake f; struct giulia_pcie_link s; struct giulia_pcie_link_io io;
    for (unsigned n = 0; n < 8; n++) {
        for (unsigned inbound = 0; inbound < 2; inbound++) {
            init(&f, &s); io = io_for(&f);
            f.regs[GIULIA_LINK_ATU][(n * 0x200 + inbound * 0x100 + 4) / 4] = BIT(31);
            assert(prepare_fixture(&s, GIULIA_LINK_PARF_REQUIRED, &io) == GIULIA_LINK_BUSY);
            assert(f.core_writes == 0 && s.state == GIULIA_LINK_STOPPED);
            assert(memcmp(f.regs[GIULIA_LINK_PARF], f.original[GIULIA_LINK_PARF],
                sizeof(f.regs[GIULIA_LINK_PARF])) == 0);
        }
        init(&f, &s); io = io_for(&f);
        f.regs[GIULIA_LINK_DBI][(0x828 + n * 12) / 4] = 1;
        assert(prepare_fixture(&s, GIULIA_LINK_PARF_REQUIRED, &io) == GIULIA_LINK_BUSY);
        assert(f.core_writes == 0 && s.state == GIULIA_LINK_STOPPED);
    }
    init(&f, &s); io = io_for(&f);
    f.regs[GIULIA_LINK_PARF][0x1b0 / 4] = BIT(8);
    assert(prepare_fixture(&s, GIULIA_LINK_PARF_REQUIRED, &io) == GIULIA_LINK_BUSY);
    assert(f.writes == 0);
    init(&f, &s); io = io_for(&f); seed16(&f, GIULIA_LINK_DBI, 4, BIT(2));
    assert(prepare_fixture(&s, GIULIA_LINK_PARF_REQUIRED, &io) == GIULIA_LINK_BUSY);
    assert(f.core_writes == 0 && s.state == GIULIA_LINK_STOPPED);
    init(&f, &s); io = io_for(&f); f.regs[GIULIA_LINK_PARF][0x22c / 4] = BIT(13);
    assert(prepare_fixture(&s, GIULIA_LINK_PARF_REQUIRED, &io) == GIULIA_LINK_BUSY);
    assert(f.writes == 0);
    /* After an exclusive SM8650 BCR reset, PARF_INT_MASK resets non-zero:
     * COLD_RESET_MASK snapshots it, clears it to 0, and restores it on stop. */
    init(&f, &s); io = io_for(&f);
    f.regs[GIULIA_LINK_PARF][0x22c / 4] = 0xfe000000u;
    f.original[GIULIA_LINK_PARF][0x22c / 4] = 0xfe000000u;
    assert(prepare_fixture(&s, GIULIA_LINK_PARF_REQUIRED | GIULIA_LINK_COLD_RESET_MASK, &io) == 0);
    assert(s.mask_int_owned && f.regs[GIULIA_LINK_PARF][0x22c / 4] == 0);
    stop(&f, &s);
    assert(f.regs[GIULIA_LINK_PARF][0x22c / 4] == 0xfe000000u);
}
static void test_timeout_and_link_loss(void)
{
    struct fake f; struct giulia_pcie_link s;
    init(&f, &s); prepare(&f, &s); f.never_link = 1;
    assert(giulia_pcie_link_train(&s, GIULIA_LINK_TRAIN_REQUIRED) == GIULIA_LINK_TIMEOUT);
    assert(s.state == GIULIA_LINK_FAULT && s.failure_stage == GIULIA_LINK_STAGE_TRAIN);
    assert(f.cfg_accesses == 0 && f.delay_total == 1000000);
    stop(&f, &s);
    init(&f, &s); prepare(&f, &s); f.drop_link_after = 1;
    assert(giulia_pcie_link_train(&s, GIULIA_LINK_TRAIN_REQUIRED) != 0);
    assert(f.cfg_accesses == 1 && s.state == GIULIA_LINK_FAULT);
    stop(&f, &s);
}
static void test_wrong_endpoint_and_capabilities(void)
{
    struct fake f; struct giulia_pcie_link s; struct giulia_pcie_link_io io;
    init(&f, &s); io = io_for(&f); seed16(&f, GIULIA_LINK_DBI, 0x70, 0x7010);
    assert(prepare_fixture(&s, GIULIA_LINK_PARF_REQUIRED, &io) == GIULIA_LINK_CAPABILITY);
    assert(f.core_writes == 0 && s.state == GIULIA_LINK_STOPPED);
    init(&f, &s); prepare(&f, &s); f.regs[GIULIA_LINK_CFG][0] = 0x123417cb;
    assert(giulia_pcie_link_train(&s, GIULIA_LINK_TRAIN_REQUIRED) == GIULIA_LINK_ENDPOINT);
    assert(f.bar_probes == 0); stop(&f, &s);
    init(&f, &s); prepare(&f, &s); seed16(&f, GIULIA_LINK_CFG, 0x50, 0x5005);
    assert(giulia_pcie_link_train(&s, GIULIA_LINK_TRAIN_REQUIRED) == GIULIA_LINK_CAPABILITY);
    assert(f.bar_probes == 0); stop(&f, &s);
    init(&f, &s); prepare(&f, &s); seed16(&f, GIULIA_LINK_CFG, 0x50, 0x7105);
    assert(giulia_pcie_link_train(&s, GIULIA_LINK_TRAIN_REQUIRED) == GIULIA_LINK_CAPABILITY);
    assert(f.bar_probes == 0); stop(&f, &s);
}
static void test_bar_and_msix(void)
{
    struct fake f; struct giulia_pcie_link s;
    init(&f, &s); prepare(&f, &s); f.bar_mask = 0xffc00004;
    assert(giulia_pcie_link_train(&s, GIULIA_LINK_TRAIN_REQUIRED) == GIULIA_LINK_BAR);
    assert(f.regs[GIULIA_LINK_CFG][0x10 / 4] == 4 && f.bar_probes == 1);
    assert(s.failure_stage == GIULIA_LINK_STAGE_BAR); stop(&f, &s);
    init(&f, &s); prepare(&f, &s);
    seed16(&f, GIULIA_LINK_CFG, 0x70, 0x9010);
    seed16(&f, GIULIA_LINK_CFG, 0x90, 0x11);
    seed16(&f, GIULIA_LINK_CFG, 0x92, 0x8007);
    assert(giulia_pcie_link_train(&s, GIULIA_LINK_TRAIN_REQUIRED) == 0);
    assert(raw16(&f, GIULIA_LINK_CFG, 0x92) == 0x4007);
    stop(&f, &s);
}
static void test_write_failures_and_retry(void)
{
    struct fake f; struct giulia_pcie_link s; struct giulia_pcie_link_io io;
    init(&f, &s); io = io_for(&f);
    f.drop_on = 1; f.drop_region = GIULIA_LINK_DBI; f.drop_offset = 0x8bc; f.drop_mask = 1;
    assert(prepare_fixture(&s, GIULIA_LINK_PARF_REQUIRED, &io) == GIULIA_LINK_READBACK);
    assert(s.state == GIULIA_LINK_FAULT); f.drop_on = 0; stop(&f, &s);
    init(&f, &s); prepare(&f, &s);
    f.drop_on = 1; f.drop_region = GIULIA_LINK_CFG; f.drop_offset = 0x52; f.drop_mask = 1;
    assert(giulia_pcie_link_train(&s, GIULIA_LINK_TRAIN_REQUIRED) == GIULIA_LINK_READBACK);
    assert(f.bar_probes == 0); f.drop_on = 0; stop(&f, &s);
    init(&f, &s); prepare(&f, &s);
    f.drop_on = 1; f.drop_region = GIULIA_LINK_ATU; f.drop_offset = 4; f.drop_mask = BIT(31);
    assert(giulia_pcie_link_train(&s, GIULIA_LINK_TRAIN_REQUIRED) == GIULIA_LINK_READBACK);
    assert(f.cfg_accesses == 0 && f.delay_total >= 45000);
    f.drop_on = 0; stop(&f, &s);
    init(&f, &s); prepare(&f, &s);
    assert(giulia_pcie_link_train(&s, GIULIA_LINK_TRAIN_REQUIRED) == 0);
    f.drop_on = 1; f.drop_region = GIULIA_LINK_PARF; f.drop_offset = 0x1b0; f.drop_mask = BIT(8);
    assert(quiesce_then_stop(&f, &s) == GIULIA_LINK_READBACK);
    assert(s.state == GIULIA_LINK_FAULT && s.saved_count != 0);
    f.drop_on = 0; stop(&f, &s);
    init(&f, &s); prepare(&f, &s);
    assert(giulia_pcie_link_train(&s, GIULIA_LINK_TRAIN_REQUIRED) == 0);
    f.check_decoders = 1;
    f.drop_on = 1; f.drop_region = GIULIA_LINK_PARF; f.drop_offset = 0;
    f.drop_mask = BIT(29); f.drop_value_only = 1; f.drop_value = f.original[GIULIA_LINK_PARF][0];
    assert(quiesce_then_stop(&f, &s) == GIULIA_LINK_READBACK);
    assert(s.state == GIULIA_LINK_FAULT && f.old_decoder);
    f.drop_on = 0; stop(&f, &s);
}
static void test_all_ones_reads(void)
{
    struct fake f; struct giulia_pcie_link s; struct giulia_pcie_link_io io;
    const unsigned regions[] = {GIULIA_LINK_PARF, GIULIA_LINK_DBI, GIULIA_LINK_ATU};
    const uint32_t offsets[] = {0, 0x710, 0x208};
    for (unsigned n = 0; n < 3; n++) {
        init(&f, &s); io = io_for(&f);
        f.fault_on = 1; f.fault_region = regions[n]; f.fault_offset = offsets[n];
        assert(prepare_fixture(&s, GIULIA_LINK_PARF_REQUIRED, &io) == GIULIA_LINK_READBACK);
        if (n == 0) assert(s.state == GIULIA_LINK_IDLE && f.writes == 0);
        else assert(s.state == GIULIA_LINK_STOPPED && f.core_writes == 0);
        for (unsigned j = 0; j < s.saved_count; j++) assert(s.saved[j].value != 0xffffffffu);
        assert(giulia_pcie_link_stop(&s, 0) == 0 && f.core_writes == 0);
    }
    /* Snapshot succeeded, then an RMW source fails: never write derived ones. */
    init(&f, &s); io = io_for(&f);
    f.fault_on = 1; f.fault_region = GIULIA_LINK_PARF; f.fault_offset = 0;
    f.fault_after_writes = 1;
    assert(prepare_fixture(&s, GIULIA_LINK_PARF_REQUIRED, &io) == GIULIA_LINK_READBACK);
    assert(s.state == GIULIA_LINK_FAULT && f.regs[GIULIA_LINK_PARF][0] == f.original[GIULIA_LINK_PARF][0]);
    f.fault_on = 0; stop(&f, &s);
    /* A masked readback with every bit set must not acknowledge RO enable. */
    init(&f, &s); io = io_for(&f);
    f.fault_on = 1; f.fault_region = GIULIA_LINK_DBI; f.fault_offset = 0x8bc;
    f.fault_required_bits = 1;
    assert(prepare_fixture(&s, GIULIA_LINK_PARF_REQUIRED, &io) == GIULIA_LINK_READBACK);
    assert(s.state == GIULIA_LINK_FAULT);
    f.fault_on = 0; stop(&f, &s);
    /* All-ones ATU enable readback is not a configured outbound window. */
    init(&f, &s); prepare(&f, &s);
    f.fault_on = 1; f.fault_region = GIULIA_LINK_ATU; f.fault_offset = 4;
    f.fault_required_bits = BIT(31);
    assert(giulia_pcie_link_train(&s, GIULIA_LINK_TRAIN_REQUIRED) == GIULIA_LINK_READBACK);
    assert(s.state == GIULIA_LINK_FAULT && f.cfg_accesses == 0);
    f.fault_on = 0; stop(&f, &s);
    /* DLLLA in an inaccessible all-ones word must never count as link up. */
    init(&f, &s); prepare(&f, &s);
    f.fault_on = 1; f.fault_region = GIULIA_LINK_DBI; f.fault_offset = 0x82;
    assert(giulia_pcie_link_train(&s, GIULIA_LINK_TRAIN_REQUIRED) == GIULIA_LINK_TIMEOUT);
    assert(s.state == GIULIA_LINK_FAULT && f.cfg_accesses == 0);
    f.fault_on = 0; stop(&f, &s);
    /* Inaccessible status is not confirmed-down during teardown either. */
    init(&f, &s); prepare(&f, &s);
    assert(giulia_pcie_link_train(&s, GIULIA_LINK_TRAIN_REQUIRED) == 0);
    f.fault_on = 1; f.fault_region = GIULIA_LINK_DBI; f.fault_offset = 0x82;
    assert(quiesce_then_stop(&f, &s) == GIULIA_LINK_READBACK);
    assert(s.state == GIULIA_LINK_FAULT && s.saved_count != 0);
    assert(f.regs[GIULIA_LINK_DBI][0x18 / 4] == 0x00010100u);
    f.fault_on = 0; stop(&f, &s);
}

static void test_cold_decoder_failures(void)
{
    struct fake f; struct giulia_pcie_link s; struct giulia_pcie_link_io io;
    const uint32_t offsets[] = {0x1000, 0x40, 0x350, 0x354, 0x634, 0x638,
        0x358, 0x35c, 0, 0x174, 0x20, 0x1a8};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        init(&f, &s); io = io_for(&f);
        f.fault_on = 1; f.fault_region = GIULIA_LINK_PARF;
        f.fault_offset = offsets[i]; f.fault_after_writes = 1;
        assert(prepare_fixture(&s, GIULIA_LINK_PARF_REQUIRED, &io) == GIULIA_LINK_READBACK);
        assert(s.state == GIULIA_LINK_FAULT && s.parf_owned && !s.core_owned);
        assert(f.core_reads == 0 && f.core_writes == 0);
        f.fault_on = 0; stop(&f, &s);
        assert(f.core_reads == 0 && f.core_writes == 0 && !s.parf_owned);
    }
    /* Discovering a foreign core never grants permission to disable it, even
     * if restoring the provisional PARF decoder subsequently fails. */
    init(&f, &s); io = io_for(&f);
    f.regs[GIULIA_LINK_DBI][0x828 / 4] = 1;
    f.drop_on = 1; f.drop_region = GIULIA_LINK_PARF; f.drop_offset = 0;
    f.drop_mask = BIT(29); f.drop_value_only = 1;
    f.drop_value = f.original[GIULIA_LINK_PARF][0];
    assert(prepare_fixture(&s, GIULIA_LINK_PARF_REQUIRED, &io) == GIULIA_LINK_READBACK);
    assert(s.state == GIULIA_LINK_FAULT && s.parf_owned && !s.core_owned);
    assert(f.core_writes == 0 && f.regs[GIULIA_LINK_DBI][0x828 / 4] == 1);
    {
        unsigned reads = f.core_reads;
        f.drop_on = 0;
        assert(giulia_pcie_link_quiesce(&s, GIULIA_LINK_QUIESCE_REQUIRED) == 0);
        assert(s.state == GIULIA_LINK_QUIESCED && !s.core_owned);
        assert(f.core_reads == reads && f.core_writes == 0);
        assert(giulia_pcie_link_stop(&s, GIULIA_LINK_STOP_REQUIRED) == 0);
        assert(f.core_reads == reads && f.core_writes == 0);
        assert(f.regs[GIULIA_LINK_DBI][0x828 / 4] == 1);
        assert(!s.parf_owned && !s.core_owned);
    }
}

static void ready_for_memory(struct fake *f, struct giulia_pcie_link *s)
{
    init(f, s); prepare(f, s);
    assert(s->parf_owned && s->core_owned);
    assert(giulia_pcie_link_train(s, GIULIA_LINK_TRAIN_REQUIRED) == 0);
    f->allow_memory = 1;
}
static void test_memory_enable_disable(void)
{
    struct fake f; struct giulia_pcie_link s;
    unsigned accesses, writes;
    ready_for_memory(&f, &s);
    accesses = f.reads + f.writes;
    assert(giulia_pcie_link_mem_enable(&s, GIULIA_LINK_MEMORY_REQUIRED &
        ~GIULIA_LINK_USERS_QUIESCED) == GIULIA_LINK_PREREQUISITE);
    assert(f.reads + f.writes == accesses);
    assert(giulia_pcie_link_mem_enable(&s, GIULIA_LINK_MEMORY_REQUIRED) == 0);
    assert(s.memory_owned && s.memory_enabled && s.state == GIULIA_LINK_READY);
    assert(strcmp(f.memory_events, "RE") == 0);
    assert((raw16(&f, GIULIA_LINK_DBI, 4) & 7u) == 2u);
    assert((raw16(&f, GIULIA_LINK_CFG, 4) & 7u) == 2u);
    assert((raw16(&f, GIULIA_LINK_CFG, 0x52) & 1u) == 0);
    assert(raw16(&f, GIULIA_LINK_DBI, 6) == 0xa510u);
    assert(raw16(&f, GIULIA_LINK_CFG, 6) == 0x4310u);
    writes = f.writes;
    assert(giulia_pcie_link_mem_enable(&s, GIULIA_LINK_MEMORY_REQUIRED) == 0);
    assert(f.writes == writes); /* Idempotent enable still verifies safety. */
    assert(giulia_pcie_link_mem_disable(&s, GIULIA_LINK_MEMORY_REQUIRED) == 0);
    assert(strcmp(f.memory_events, "REer") == 0 && !s.memory_enabled);
    assert((raw16(&f, GIULIA_LINK_DBI, 4) & 7u) == 0);
    assert((raw16(&f, GIULIA_LINK_CFG, 4) & 7u) == 0);
    stop(&f, &s);
    /* PERST# isolation also permits stop without a previous MEM-disable call.
     * The core must make no endpoint transactions once reset is asserted. */
    ready_for_memory(&f, &s);
    assert(giulia_pcie_link_mem_enable(&s, GIULIA_LINK_MEMORY_REQUIRED) == 0);
    stop(&f, &s);
    assert(!s.memory_enabled && !s.memory_owned && !s.core_owned);
}

static void test_memory_guard_failures(void)
{
    struct fake f; struct giulia_pcie_link s;
    unsigned writes, accesses;
    ready_for_memory(&f, &s);
    f.regs[GIULIA_LINK_ATU][0x14 / 4] ^= 0x01000000u;
    writes = f.writes; accesses = f.cfg_accesses;
    assert(giulia_pcie_link_mem_enable(&s, GIULIA_LINK_MEMORY_REQUIRED) == GIULIA_LINK_READBACK);
    assert(f.writes == writes && f.cfg_accesses == accesses && !s.memory_owned);
    stop(&f, &s);
    ready_for_memory(&f, &s);
    f.regs[GIULIA_LINK_ATU][0x214 / 4] ^= 0x200000;
    writes = f.writes;
    assert(giulia_pcie_link_mem_enable(&s, GIULIA_LINK_MEMORY_REQUIRED) == GIULIA_LINK_READBACK);
    assert(f.writes == writes && !s.memory_owned); stop(&f, &s);
    ready_for_memory(&f, &s); seed16(&f, GIULIA_LINK_CFG, 0x52, 0x015b);
    writes = f.writes;
    assert(giulia_pcie_link_mem_enable(&s, GIULIA_LINK_MEMORY_REQUIRED) == GIULIA_LINK_BUSY);
    assert(f.writes == writes && !s.memory_owned); stop(&f, &s);
    ready_for_memory(&f, &s); seed16(&f, GIULIA_LINK_CFG, 4, BIT(10) | BIT(2));
    writes = f.writes;
    assert(giulia_pcie_link_mem_enable(&s, GIULIA_LINK_MEMORY_REQUIRED) == GIULIA_LINK_BUSY);
    assert(f.writes == writes && !s.memory_owned); stop(&f, &s);
    ready_for_memory(&f, &s); f.regs[GIULIA_LINK_CFG][0x10 / 4] = 0x60600004;
    writes = f.writes;
    assert(giulia_pcie_link_mem_enable(&s, GIULIA_LINK_MEMORY_REQUIRED) == GIULIA_LINK_BAR);
    assert(f.writes == writes && !s.memory_owned); stop(&f, &s);
}

static void test_memory_readback_and_retry(void)
{
    struct fake f; struct giulia_pcie_link s;
    for (unsigned r = GIULIA_LINK_DBI; r <= GIULIA_LINK_CFG; r += 2) {
        ready_for_memory(&f, &s);
        f.drop_on = 1; f.drop_region = r; f.drop_offset = 4; f.drop_mask = BIT(1);
        assert(giulia_pcie_link_mem_enable(&s, GIULIA_LINK_MEMORY_REQUIRED) == GIULIA_LINK_READBACK);
        assert(s.state == GIULIA_LINK_FAULT && s.memory_owned && !s.memory_enabled);
        assert((raw16(&f, GIULIA_LINK_DBI, 4) & 7u) == 0);
        assert((raw16(&f, GIULIA_LINK_CFG, 4) & 7u) == 0);
        f.drop_on = 0;
        assert(giulia_pcie_link_mem_disable(&s, GIULIA_LINK_MEMORY_REQUIRED) == 0);
        assert(s.state == GIULIA_LINK_FAULT); /* No implicit fault dismissal. */
        stop(&f, &s);
    }
    ready_for_memory(&f, &s);
    assert(giulia_pcie_link_mem_enable(&s, GIULIA_LINK_MEMORY_REQUIRED) == 0);
    f.drop_on = 1; f.drop_region = GIULIA_LINK_CFG; f.drop_offset = 4; f.drop_mask = BIT(1);
    assert(giulia_pcie_link_mem_disable(&s, GIULIA_LINK_MEMORY_REQUIRED) == GIULIA_LINK_READBACK);
    assert(s.state == GIULIA_LINK_FAULT && s.memory_owned);
    assert((raw16(&f, GIULIA_LINK_DBI, 4) & 7u) == 0);
    assert((raw16(&f, GIULIA_LINK_CFG, 4) & 7u) == 2u);
    f.drop_on = 0;
    assert(giulia_pcie_link_mem_disable(&s, GIULIA_LINK_MEMORY_REQUIRED) == 0);
    assert(!s.memory_enabled); stop(&f, &s);
    ready_for_memory(&f, &s);
    assert(giulia_pcie_link_mem_enable(&s, GIULIA_LINK_MEMORY_REQUIRED) == 0);
    f.lost_link = 1;
    {
        unsigned accesses = f.cfg_accesses;
        assert(giulia_pcie_link_mem_disable(&s, GIULIA_LINK_MEMORY_REQUIRED) == GIULIA_LINK_DOWN);
        assert(f.cfg_accesses == accesses && s.state == GIULIA_LINK_FAULT);
        assert((raw16(&f, GIULIA_LINK_DBI, 4) & 7u) == 0);
    }
    stop(&f, &s);
}
static void test_deferred_parf_restore(void)
{
    struct fake f; struct giulia_pcie_link s;
    unsigned writes;
    ready_for_memory(&f, &s);
    assert(giulia_pcie_link_mem_enable(&s, GIULIA_LINK_MEMORY_REQUIRED) == 0);
    f.perst_asserted = 1;
    writes = f.writes;
    assert(giulia_pcie_link_stop(&s, GIULIA_LINK_STOP_REQUIRED) == GIULIA_LINK_STATE);
    assert(giulia_pcie_link_quiesce(&s, GIULIA_LINK_QUIESCE_REQUIRED) == GIULIA_LINK_PREREQUISITE);
    assert(f.writes == writes);
    assert(giulia_pcie_link_quiesce(&s, GIULIA_LINK_QUIESCE_REQUIRED | GIULIA_LINK_PHY_READY) == 0);
    assert(s.state == GIULIA_LINK_QUIESCED && s.parf_owned && !s.core_owned);
    assert(!s.memory_enabled && !s.config_atu_enabled);
    assert((f.regs[GIULIA_LINK_PARF][0x40 / 4] & 1u) == 0);
    assert(f.regs[GIULIA_LINK_DBI][0x18 / 4] == f.original[GIULIA_LINK_DBI][0x18 / 4]);
    writes = f.writes;
    assert(giulia_pcie_link_stop(&s, GIULIA_LINK_QUIESCE_REQUIRED) == GIULIA_LINK_PREREQUISITE);
    assert(giulia_pcie_link_quiesce(&s, GIULIA_LINK_QUIESCE_REQUIRED) == 0);
    assert(f.writes == writes); /* QMP can now stop while PARF remains held. */
    f.core_dark = 1;
    stop(&f, &s);
    assert((f.regs[GIULIA_LINK_PARF][0x40 / 4] & 1u) == 1u);
}

static void test_split_prepare(void)
{
    struct fake f;
    struct giulia_pcie_link s;
    struct giulia_pcie_link_io io;
    unsigned writes;
    init(&f, &s); io = io_for(&f);
    f.core_dark = 1;
    assert(giulia_pcie_link_prepare(&s, GIULIA_LINK_PREPARE_REQUIRED) == GIULIA_LINK_STATE);
    assert(giulia_pcie_link_prepare_parf(&s, GIULIA_LINK_PARF_REQUIRED, &io) == 0);
    assert(s.state == GIULIA_LINK_PARF_READY && !f.core_reads && !f.core_writes);
    assert(giulia_pcie_link_prepare(&s, GIULIA_LINK_PARF_REQUIRED) == GIULIA_LINK_PREREQUISITE);
    f.core_dark = 0;
    f.regs[GIULIA_LINK_DBI][0] = 0xffffffffu;
    writes = f.writes;
    assert(giulia_pcie_link_prepare(&s, GIULIA_LINK_PREPARE_REQUIRED) == GIULIA_LINK_READBACK);
    assert(s.parf_owned && !s.core_owned && f.writes == writes && f.core_reads == 1);
    assert(giulia_pcie_link_quiesce(&s, GIULIA_LINK_QUIESCE_REQUIRED | GIULIA_LINK_PHY_READY) == 0);
    assert(f.writes == writes && f.core_reads == 1);
    f.core_dark = 1;
    assert(giulia_pcie_link_stop(&s, GIULIA_LINK_STOP_REQUIRED) == 0 && f.core_reads == 1);
    assert(!memcmp(f.regs[GIULIA_LINK_PARF], f.original[GIULIA_LINK_PARF],
        sizeof(f.regs[GIULIA_LINK_PARF])));
    init(&f, &s); io = io_for(&f);
    assert(giulia_pcie_link_prepare_parf(&s, GIULIA_LINK_PARF_REQUIRED, &io) == 0);
    writes = f.writes;
    assert(giulia_pcie_link_read_root_id(&s, GIULIA_LINK_PREPARE_REQUIRED) == 0);
    assert(s.root_id == 0x011317cbu && s.state == GIULIA_LINK_PARF_READY);
    assert(f.core_reads == 1 && f.writes == writes && !s.core_owned);
    puts("Ace5 PCIe PARF-before-PHY / DBI-after-PIPE and PARF-only shutdown passed");
}

static void test_parf_drift_before_dbi(void)
{
    const uint32_t offsets[] = {0x350u, 0x634u, 0x358u, 0x1b0u, 0x22cu};
    const uint32_t changed[] = {0x60000000u, 0xffffffffu, 0u, BIT(8), BIT(13)};
    const int expected[] = {GIULIA_LINK_READBACK, GIULIA_LINK_READBACK,
        GIULIA_LINK_READBACK, GIULIA_LINK_BUSY, GIULIA_LINK_BUSY};
    for (unsigned index = 0; index < sizeof(offsets) / sizeof(offsets[0]); index++) {
        struct fake f;
        struct giulia_pcie_link s;
        init(&f, &s);
        struct giulia_pcie_link_io io = io_for(&f);
        f.core_dark = 1;
        assert(giulia_pcie_link_prepare_parf(&s, GIULIA_LINK_PARF_REQUIRED, &io) == 0);
        unsigned writes = f.writes;
        f.regs[GIULIA_LINK_PARF][offsets[index] / 4u] = changed[index];
        assert(giulia_pcie_link_prepare(&s, GIULIA_LINK_PREPARE_REQUIRED) == expected[index]);
        assert(s.failure_stage == GIULIA_LINK_STAGE_PARF && f.writes == writes);
        assert(!f.core_reads && !f.core_writes && s.parf_owned && !s.core_owned);
    }
}
static void test_boot_profile(void)
{
    struct fake f; struct giulia_pcie_link s; struct giulia_pcie_link_io io;
    const uint16_t bad[] = {0x12, 0x13, 0x21, 0x10, 0x01, 0x00, 0x3f1};
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        init(&f, &s); prepare(&f, &s);
        f.negotiated_profile = bad[i];
        assert(giulia_pcie_link_train(&s, GIULIA_LINK_TRAIN_REQUIRED) == GIULIA_LINK_PROFILE);
        assert(s.state == GIULIA_LINK_FAULT && s.failure_stage == GIULIA_LINK_STAGE_TRAIN);
        assert((s.link_status & 0x3ffu) == bad[i]);
        assert(f.cfg_accesses == 0 && !s.atu_touched && !s.config_atu_enabled);
        stop(&f, &s); /* Down polling must not require the old valid profile. */
    }
    /* Loss of the bounded profile blocks config access even after training. */
    ready_for_memory(&f, &s);
    f.negotiated_profile = 0x21;
    unsigned cfg = f.cfg_accesses, writes = f.writes;
    assert(giulia_pcie_link_mem_enable(&s, GIULIA_LINK_MEMORY_REQUIRED) == GIULIA_LINK_PROFILE);
    assert(f.cfg_accesses == cfg && f.writes == writes && !s.memory_owned);
    stop(&f, &s);
    ready_for_memory(&f, &s);
    assert(giulia_pcie_link_mem_enable(&s, GIULIA_LINK_MEMORY_REQUIRED) == 0);
    f.negotiated_profile = 0x12; cfg = f.cfg_accesses;
    assert(giulia_pcie_link_mem_disable(&s, GIULIA_LINK_MEMORY_REQUIRED) == GIULIA_LINK_PROFILE);
    assert(f.cfg_accesses == cfg && (raw16(&f, GIULIA_LINK_DBI, 4) & 7u) == 0);
    assert(s.memory_owned && s.state == GIULIA_LINK_FAULT);
    stop(&f, &s);
    init(&f, &s); prepare(&f, &s); f.profile_drift_after_cfg = 1;
    assert(giulia_pcie_link_train(&s, GIULIA_LINK_TRAIN_REQUIRED) != 0);
    assert(f.cfg_accesses == 1 && s.state == GIULIA_LINK_FAULT);
    stop(&f, &s);

    /* 16-bit control updates must not acknowledge adjacent Link Status 2. */
    const uint32_t offsets[] = {0x710, 0x80c, 0x7c, 0xa0};
    const uint32_t masks[] = {0x3f0000, 0x1f00, 0x3ff, 0x0f};
    for (unsigned i = 0; i < sizeof(offsets)/sizeof(offsets[0]); i++) {
        init(&f, &s); io = io_for(&f);
        f.drop_on = 1; f.drop_region = GIULIA_LINK_DBI;
        f.drop_offset = offsets[i]; f.drop_mask = masks[i];
        assert(prepare_fixture(&s, GIULIA_LINK_PARF_REQUIRED, &io) == GIULIA_LINK_READBACK);
        assert(s.state == GIULIA_LINK_FAULT && s.core_owned && f.cfg_accesses == 0);
        assert(raw16(&f, GIULIA_LINK_DBI, 0xa2) == 0x8090);
        f.drop_on = 0; stop(&f, &s);
    }
    init(&f, &s); prepare(&f, &s);
    f.drop_on = 1; f.drop_region = GIULIA_LINK_DBI; f.drop_offset = 0xa0;
    f.drop_mask = 0xf; f.perst_asserted = 1;
    assert(quiesce_then_stop(&f, &s) == GIULIA_LINK_READBACK);
    assert(s.state == GIULIA_LINK_FAULT && s.core_owned && s.saved_count);
    f.drop_on = 0; stop(&f, &s);

    /* Missing v2 and an out-of-range v2 capability are read-only rejects. */
    init(&f, &s); io = io_for(&f); seed16(&f, GIULIA_LINK_DBI, 0x72, 0x41);
    assert(prepare_fixture(&s, GIULIA_LINK_PARF_REQUIRED, &io) == GIULIA_LINK_CAPABILITY);
    assert(f.core_writes == 0 && !s.core_owned && !s.parf_owned);
    init(&f, &s); io = io_for(&f);
    seed16(&f, GIULIA_LINK_DBI, 0x34, 0xd0);
    seed16(&f, GIULIA_LINK_DBI, 0xd0, 0x10);
    seed16(&f, GIULIA_LINK_DBI, 0xd2, 0x42);
    assert(prepare_fixture(&s, GIULIA_LINK_PARF_REQUIRED, &io) == GIULIA_LINK_CAPABILITY);
    assert(f.core_writes == 0 && !s.core_owned && !s.parf_owned);
    puts("Ace5 PCIe boot profile: Gen1 x1 programming, status rejection, config drift and retry restore passed");
}

int main(void)
{
    test_downstream_aperture();
    test_default_and_gates();
    test_success();
    test_busy();
    test_timeout_and_link_loss();
    test_wrong_endpoint_and_capabilities();
    test_bar_and_msix();
    test_write_failures_and_retry();
    test_all_ones_reads();
    test_cold_decoder_failures();
    test_memory_enable_disable();
    test_memory_guard_failures();
    test_memory_readback_and_retry();
    test_deferred_parf_restore();
    test_split_prepare();
    test_parf_drift_before_dbi();
    test_boot_profile();
    puts("SM8650 PCIe link: ownership, training, config, BAR and rollback mocks passed");
    return 0;
}
