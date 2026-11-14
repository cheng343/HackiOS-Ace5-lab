/* SPDX-License-Identifier: BSD-2-Clause */
#include "giulia_mhi_snapshot.h"

/* Original read-only implementation using public register facts, not Linux
 * driver code. Linux v6.12 drivers/bus/mhi/common.h:13-123;
 * host/init.c:1133-1162 (full u32 offsets),1165-1172 (undefined BHIE RX POR);
 * drivers/net/wireless/ath/ath12k/mhi.c:298-304,329-330 (direct BAR0 reads).
 * https://github.com/torvalds/linux/blob/v6.12/drivers/bus/mhi/common.h
 * https://github.com/torvalds/linux/blob/v6.12/drivers/bus/mhi/host/init.c
 * https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/mhi.c
 */
static const uint32_t header_offsets[GIULIA_MHI_HEADER_WORDS] = {
    0x00u, 0x08u, 0x10u, 0x28u, 0x2cu, 0x38u, 0x48u
};
static const uint32_t bhi_offsets[GIULIA_MHI_BHI_WORDS] = {
    0x00u, 0x04u, 0x28u, 0x2cu, 0x30u
};
static const uint32_t bhie_offsets[GIULIA_MHI_BHIE_WORDS] = { 0x44u, 0x78u };
/* Entire known register footprints exclude overlap/aliasing into MHI or
 * BHI identity words, even though only a smaller explicit whitelist is read.
 * These are conservative admission bounds, not a claim that other layouts
 * cannot exist. BHI ends at RSVD5+4, BHIE at RXVECSTATUS+4. */
#define MHI_FOOTPRINT 0xa8u
#define BHI_FOOTPRINT 0xc8u
#define BHIE_FOOTPRINT 0x7cu

static int read_one(const struct giulia_mhi_snapshot_io *io,
    struct giulia_mhi_snapshot *s, uint32_t offset, uint32_t *value)
{
    int err;
    if ((offset & 3u) || offset > io->bar_bytes - 4u)
        return GIULIA_MHI_SNAPSHOT_LAYOUT;
    s->read_count++;
    err = io->read32(io->cookie, offset, value);
    if (!err) return GIULIA_MHI_SNAPSHOT_OK;
    s->failed_offset = offset;
    s->callback_error = err;
    return GIULIA_MHI_SNAPSHOT_READ_ERROR;
}

static int read_words(const struct giulia_mhi_snapshot_io *io,
    struct giulia_mhi_snapshot *s, uint32_t base, const uint32_t *offsets,
    unsigned count, uint32_t *dest)
{
    unsigned i;
    int err;
    for (i = 0; i < count; i++) {
        err = read_one(io, s, base + offsets[i], &dest[i]);
        if (err) return err;
    }
    return GIULIA_MHI_SNAPSHOT_OK;
}

static int layout(const struct giulia_mhi_snapshot_io *io, const uint32_t *h)
{
    uint32_t bhi = h[GIULIA_MHI_BHI_OFFSET], bhie = h[GIULIA_MHI_BHIE_OFFSET];
    unsigned i;
    for (i = 0; i < GIULIA_MHI_HEADER_WORDS; i++)
        if (h[i] == 0xffffffffu) return GIULIA_MHI_SNAPSHOT_UNAVAILABLE;
    if ((bhi & 3u) || (bhie & 3u) || bhi < MHI_FOOTPRINT || bhie < MHI_FOOTPRINT ||
        bhi > io->bar_bytes - BHI_FOOTPRINT ||
        bhie > io->bar_bytes - BHIE_FOOTPRINT)
        return GIULIA_MHI_SNAPSHOT_LAYOUT;
    if (bhi < bhie + BHIE_FOOTPRINT && bhie < bhi + BHI_FOOTPRINT)
        return GIULIA_MHI_SNAPSHOT_LAYOUT;
    return GIULIA_MHI_SNAPSHOT_OK;
}

static int changed(const uint32_t *a, const uint32_t *b, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++) if (a[i] != b[i]) return 1;
    return 0;
}

int giulia_mhi_snapshot(const struct giulia_mhi_snapshot_io *io,
    uint32_t flags, struct giulia_mhi_snapshot *s)
{
    uint32_t bhi, bhie;
    int err;
    if (!s) return GIULIA_MHI_SNAPSHOT_ARGUMENT;
    *s = (struct giulia_mhi_snapshot){0};
    if (!io || !io->read32) return GIULIA_MHI_SNAPSHOT_ARGUMENT;
    if ((flags & GIULIA_MHI_SNAPSHOT_REQUIRED) != GIULIA_MHI_SNAPSHOT_REQUIRED)
        return GIULIA_MHI_SNAPSHOT_PREREQUISITE;
    if (io->bar_bytes != GIULIA_MHI_BAR_BYTES) return GIULIA_MHI_SNAPSHOT_LAYOUT;
    err = read_words(io, s, 0, header_offsets, GIULIA_MHI_HEADER_WORDS, s->header);
    if (err) return err;
    s->valid |= GIULIA_MHI_SNAPSHOT_HAVE_HEADER;
    err = layout(io, s->header);
    if (err) return err;
    bhi = s->header[GIULIA_MHI_BHI_OFFSET];
    bhie = s->header[GIULIA_MHI_BHIE_OFFSET];
    err = read_words(io, s, bhi, bhi_offsets, GIULIA_MHI_BHI_WORDS, s->bhi);
    if (err) return err;
    s->valid |= GIULIA_MHI_SNAPSHOT_HAVE_BHI;
    err = read_words(io, s, bhie, bhie_offsets, GIULIA_MHI_BHIE_WORDS, s->bhie);
    if (err) return err;
    s->valid |= GIULIA_MHI_SNAPSHOT_HAVE_BHIE;
    err = read_words(io, s, 0, header_offsets, GIULIA_MHI_HEADER_WORDS, s->header_after);
    if (err) return err;
    if (changed(s->header, s->header_after, GIULIA_MHI_HEADER_WORDS))
        s->valid |= GIULIA_MHI_SNAPSHOT_CHANGED;
    err = layout(io, s->header_after);
    if (err) return err;
    /* Do not follow a newly reported block address, or reread an old one
     * after its meaning changed. Keep captured before/header-after evidence. */
    if (bhi != s->header_after[GIULIA_MHI_BHI_OFFSET] ||
        bhie != s->header_after[GIULIA_MHI_BHIE_OFFSET])
        return GIULIA_MHI_SNAPSHOT_LAYOUT;
    err = read_words(io, s, bhi, bhi_offsets, GIULIA_MHI_BHI_WORDS, s->bhi_after);
    if (err) return err;
    err = read_words(io, s, bhie, bhie_offsets, GIULIA_MHI_BHIE_WORDS, s->bhie_after);
    if (err) return err;
    s->valid |= GIULIA_MHI_SNAPSHOT_HAVE_AFTER;
    if (changed(s->bhi, s->bhi_after, GIULIA_MHI_BHI_WORDS) ||
        changed(s->bhie, s->bhie_after, GIULIA_MHI_BHIE_WORDS))
        s->valid |= GIULIA_MHI_SNAPSHOT_CHANGED;
    return GIULIA_MHI_SNAPSHOT_OK;
}
