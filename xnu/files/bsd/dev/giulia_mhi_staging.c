/* SPDX-License-Identifier: BSD-2-Clause */
/* Coherent firmware staging, keyed to a pinned KIWI DMA-domain provider.
 * No MMIO, kernel address translation, OS allocator or default provider. */
#include "giulia_mhi_staging.h"

static int gs_span(uint64_t base, uint64_t size, uint64_t limit)
{
    return size && base <= limit && size - 1u <= limit - base;
}

static int gs_overlap(uint64_t a, uint64_t na, uint64_t b, uint64_t nb)
{
    return a <= b ? b - a < na : a - b < nb;
}

static uint64_t gs_cpu(const void *p)
{
    return (uint64_t)(uintptr_t)p;
}

static int gs_live(struct giulia_mhi_stage_state *s)
{
    return s->owner && !s->ops.live(s->ops.cookie, s->owner);
}

static int gs_owned(struct giulia_mhi_stage_state *s, uint32_t i)
{
    return gs_live(s) && !s->ops.owns(s->ops.cookie, s->owner, &s->slots[i].buffer);
}

static int gs_retained(struct giulia_mhi_stage_state *s, int error)
{
    s->phase = GIULIA_MHI_STAGE_RETAINED;
    return error;
}

int giulia_mhi_stage_release(struct giulia_mhi_stage_state *s)
{
    if (!s) return GIULIA_MHI_STAGE_ARGUMENT;
    if (s->phase == GIULIA_MHI_STAGE_EMPTY) return GIULIA_MHI_STAGE_OK;
    if (s->phase == GIULIA_MHI_STAGE_POISONED) return GIULIA_MHI_STAGE_PROVIDER;
    if (s->phase == GIULIA_MHI_STAGE_FLIGHT) {
        /* Remain in FLIGHT until positively retired; even a transient loss
         * must never turn the next release into an ungated retry. */
        if (!gs_live(s)) return GIULIA_MHI_STAGE_DOMAIN;
        if (s->ops.quiesced(s->ops.cookie, s->owner)) return GIULIA_MHI_STAGE_BUSY;
    } else if (s->phase != GIULIA_MHI_STAGE_RETAINED &&
        s->phase != GIULIA_MHI_STAGE_PREPARING && s->phase != GIULIA_MHI_STAGE_READY) {
        return GIULIA_MHI_STAGE_STATE;
    }
    s->phase = GIULIA_MHI_STAGE_RETAINED;
    while (s->count) {
        uint32_t i = s->count - 1u;
        if (!gs_owned(s, i)) return GIULIA_MHI_STAGE_DOMAIN;
        if (s->ops.free(s->ops.cookie, s->owner, s->slots[i].buffer.handle))
            return GIULIA_MHI_STAGE_CLEANUP;
        s->slots[i] = (struct giulia_mhi_stage_slot){0};
        s->count = i; /* no subsequent retry can free the released lease */
    }
    if (!gs_live(s)) return GIULIA_MHI_STAGE_DOMAIN;
    if (s->ops.unpin(s->ops.cookie, s->owner)) return GIULIA_MHI_STAGE_CLEANUP;
    __builtin_memset(s, 0, sizeof(*s));
    return GIULIA_MHI_STAGE_OK;
}

static int gs_stop(struct giulia_mhi_stage_state *s, int cause)
{
    if (s->phase == GIULIA_MHI_STAGE_POISONED) return GIULIA_MHI_STAGE_PROVIDER;
    int rc = giulia_mhi_stage_release(s);
    return rc ? rc : cause;
}

static int gs_buffer_valid(const struct giulia_mhi_stage_state *s, uint32_t i,
    const struct giulia_mhi_stage_ops *source_ops, const uint8_t *image, uint32_t align)
{
    const struct giulia_mhi_stage_buffer *b = &s->slots[i].buffer;
    uint64_t a = gs_cpu(b->cpu), n = b->bytes, used = s->slots[i].used;
    uint64_t max = (used + GIULIA_MHI_STAGE_GRANULE - 1u) &
        ~(uint64_t)(GIULIA_MHI_STAGE_GRANULE - 1u);
    if (!b->cpu || n < used || n > max ||
        !gs_span(a, n, ((uint64_t)(~(uintptr_t)0))) || b->iova < GIULIA_MHI_STAGE_FIRST ||
        !gs_span(b->iova, n, GIULIA_MHI_STAGE_LAST) || (b->iova & (align - 1u)))
        return 0;
    /* Distinct CPU/IOVA spaces: this platform policy explicitly refuses a
     * numerically identical or overlapping CPU range passed off as an IOVA. */
    if (gs_overlap(a, n, b->iova, n) || gs_overlap(a, n, gs_cpu(s), sizeof(*s)) ||
        gs_overlap(a, n, gs_cpu(source_ops), sizeof(*source_ops)) ||
        gs_overlap(a, n, gs_cpu(image), s->plan.image_bytes))
        return 0;
    for (uint32_t j = 0; j < i; j++) {
        const struct giulia_mhi_stage_buffer *old = &s->slots[j].buffer;
        if (gs_overlap(a, n, gs_cpu(old->cpu), old->bytes) ||
            gs_overlap(b->iova, n, old->iova, old->bytes) ||
            gs_overlap(a, n, old->iova, old->bytes) ||
            gs_overlap(b->iova, n, gs_cpu(old->cpu), old->bytes)) return 0;
    }
    return 1;
}

static int gs_alloc(struct giulia_mhi_stage_state *s, uint32_t used, uint32_t align,
    const struct giulia_mhi_stage_ops *source_ops, const uint8_t *image)
{
    uint32_t i = s->count;
    if (!used || i >= GIULIA_MHI_STAGE_SLOTS) return GIULIA_MHI_STAGE_STATE;
    if (!gs_live(s)) return GIULIA_MHI_STAGE_DOMAIN;
    struct giulia_mhi_stage_buffer desc = {0};
    int rc = s->ops.alloc(s->ops.cookie, s->owner, used, align, &desc);
    if (rc && !desc.handle && !desc.cpu && !desc.iova && !desc.bytes)
        return GIULIA_MHI_STAGE_ALLOC;
    /* Save even broken results for external reconciliation: no freeing on
     * an ambiguous acquisition or on indistinguishable lease handles. */
    s->slots[i].buffer = desc;
    s->slots[i].used = used;
    s->count = i + 1u;
    if (rc || !desc.handle) goto poison;
    for (uint32_t j = 0; j < i; j++)
        if (s->slots[j].buffer.handle == desc.handle) goto poison;
    if (!gs_owned(s, i)) return GIULIA_MHI_STAGE_DOMAIN;
    if (!gs_buffer_valid(s, i, source_ops, image, align)) return GIULIA_MHI_STAGE_MAPPING;
    return GIULIA_MHI_STAGE_OK;
poison:
    s->phase = GIULIA_MHI_STAGE_POISONED;
    return GIULIA_MHI_STAGE_PROVIDER;
}

int giulia_mhi_stage_prepare(struct giulia_mhi_stage_state *s,
    const struct giulia_mhi_stage_ops *ops, const uint8_t *image, uint64_t bytes)
{
    struct giulia_mhi_fw_plan plan;
    if (!s || !ops || !image || giulia_mhi_fw_plan(bytes, &plan) ||
        !gs_span(gs_cpu(image), bytes, ((uint64_t)(~(uintptr_t)0))) ||
        !gs_span(gs_cpu(s), sizeof(*s), ((uint64_t)(~(uintptr_t)0))) ||
        !gs_span(gs_cpu(ops), sizeof(*ops), ((uint64_t)(~(uintptr_t)0))) ||
        gs_overlap(gs_cpu(s), sizeof(*s), gs_cpu(ops), sizeof(*ops)) ||
        gs_overlap(gs_cpu(s), sizeof(*s), gs_cpu(image), bytes) ||
        gs_overlap(gs_cpu(ops), sizeof(*ops), gs_cpu(image), bytes))
        return GIULIA_MHI_STAGE_ARGUMENT;
    if (s->phase != GIULIA_MHI_STAGE_EMPTY || s->owner || s->count)
        return GIULIA_MHI_STAGE_STATE;
    if (!ops->pin || !ops->live || !ops->owns || !ops->alloc || !ops->sync ||
        !ops->free || !ops->quiesced || !ops->unpin) return GIULIA_MHI_STAGE_ARGUMENT;
    s->ops = *ops;
    s->plan = plan;
    s->phase = GIULIA_MHI_STAGE_PREPARING;
    int rc = s->ops.pin(s->ops.cookie, GIULIA_MHI_STAGE_SID, GIULIA_MHI_STAGE_FIRST,
        GIULIA_MHI_STAGE_LAST, &s->owner);
    if (rc && !s->owner) {
        __builtin_memset(s, 0, sizeof(*s));
        return GIULIA_MHI_STAGE_DOMAIN;
    }
    if (rc || !s->owner) {
        s->phase = GIULIA_MHI_STAGE_POISONED;
        return GIULIA_MHI_STAGE_PROVIDER;
    }
    rc = gs_alloc(s, plan.bhi_bytes, 4u, ops, image);
    if (rc) return gs_stop(s, rc);
    for (uint32_t j = 0; j < plan.data_segments; j++) {
        uint64_t off;
        uint32_t n;
        if (giulia_mhi_fw_segment(bytes, j, &off, &n)) return gs_stop(s, GIULIA_MHI_STAGE_STATE);
        rc = gs_alloc(s, n, 4u, ops, image);
        if (rc) return gs_stop(s, rc);
        const struct giulia_mhi_stage_buffer *b = &s->slots[j + 1u].buffer;
        s->vectors[j] = (struct giulia_mhi_fw_mapping){ .addr = b->iova, .bytes = b->bytes };
    }
    rc = gs_alloc(s, plan.table_bytes, 8u, ops, image);
    if (rc) return gs_stop(s, rc);

    /* All mappings, including the last, have been admitted before the first
     * write. There is a separate BHI lease; BHIE segment 0 repeats offset 0. */
    uint32_t last = s->count - 1u;
    for (uint32_t j = 0; j <= last; j++) {
        struct giulia_mhi_stage_slot *slot = &s->slots[j];
        if (!gs_owned(s, j)) return gs_stop(s, GIULIA_MHI_STAGE_DOMAIN);
        if (j == last) {
            struct giulia_mhi_fw_mapping table = { .addr = slot->buffer.iova, .bytes = slot->buffer.bytes };
            if (giulia_mhi_fw_build_table(bytes, s->vectors, plan.data_segments, &table,
                GIULIA_MHI_STAGE_FIRST, GIULIA_MHI_STAGE_LAST, slot->buffer.cpu, slot->used))
                return gs_stop(s, GIULIA_MHI_STAGE_MAPPING);
        } else {
            uint64_t off = 0;
            uint32_t n = plan.bhi_bytes;
            if (j && giulia_mhi_fw_segment(bytes, j - 1u, &off, &n))
                return gs_stop(s, GIULIA_MHI_STAGE_STATE);
            __builtin_memcpy(slot->buffer.cpu, image + off, n);
        }
        if (s->ops.sync(s->ops.cookie, s->owner, &slot->buffer, slot->used))
            return gs_stop(s, GIULIA_MHI_STAGE_SYNC);
    }
    if (!gs_live(s)) return gs_stop(s, GIULIA_MHI_STAGE_DOMAIN);
    s->phase = GIULIA_MHI_STAGE_READY;
    return GIULIA_MHI_STAGE_OK;
}

int giulia_mhi_stage_submit(struct giulia_mhi_stage_state *s, struct giulia_mhi_stage_view *view)
{
    if (!s || !view) return GIULIA_MHI_STAGE_ARGUMENT;
    if (s->phase != GIULIA_MHI_STAGE_READY) return GIULIA_MHI_STAGE_STATE;
    uint64_t addr = gs_cpu(view);
    if (!gs_span(addr, sizeof(*view), ((uint64_t)(~(uintptr_t)0))) ||
        gs_overlap(addr, sizeof(*view), gs_cpu(s), sizeof(*s))) return GIULIA_MHI_STAGE_ARGUMENT;
    for (uint32_t i = 0; i < s->count; i++) {
        if (gs_overlap(addr, sizeof(*view), gs_cpu(s->slots[i].buffer.cpu),
            s->slots[i].buffer.bytes)) return GIULIA_MHI_STAGE_ARGUMENT;
        if (!gs_owned(s, i)) return gs_retained(s, GIULIA_MHI_STAGE_DOMAIN);
    }
    uint32_t last = s->count - 1u;
    s->phase = GIULIA_MHI_STAGE_FLIGHT;
    *view = (struct giulia_mhi_stage_view){ .bhi_iova = s->slots[0].buffer.iova,
        .bhie_iova = s->slots[last].buffer.iova, .bhi_bytes = s->plan.bhi_bytes,
        .bhie_bytes = s->plan.table_bytes };
    return GIULIA_MHI_STAGE_OK;
}
