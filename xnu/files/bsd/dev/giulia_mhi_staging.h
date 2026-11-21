/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_MHI_STAGING_H
#define GIULIA_MHI_STAGING_H
#include "giulia_mhi_firmware.h"

/* PKG110 01:00.0 KIWI, not the RC, IPA, or an MSI aperture. The provider
 * must positively identify this board, requester, routing and translating
 * domain. Knowing these three numbers, or a CPU physical address, is NOT an
 * implementation of pin/owns. This interface has no bypass backend. */
#define GIULIA_MHI_STAGE_SID 0x1401u
#define GIULIA_MHI_STAGE_FIRST 0x98000000ULL
#define GIULIA_MHI_STAGE_LAST  0xafffffffULL
#define GIULIA_MHI_STAGE_SLOTS (GIULIA_MHI_FW_MAX_SEGMENTS + 2u)
/* Driver policy: accept coherent extents up to the requested byte count
 * rounded up to 64 KiB. Only requested bytes are touched/published. */
#define GIULIA_MHI_STAGE_GRANULE 65536u

enum giulia_mhi_stage_phase {
    GIULIA_MHI_STAGE_EMPTY, GIULIA_MHI_STAGE_PREPARING, GIULIA_MHI_STAGE_READY,
    GIULIA_MHI_STAGE_FLIGHT, GIULIA_MHI_STAGE_RETAINED, GIULIA_MHI_STAGE_POISONED
};
enum giulia_mhi_stage_result {
    GIULIA_MHI_STAGE_OK = 0, GIULIA_MHI_STAGE_ARGUMENT = -1,
    GIULIA_MHI_STAGE_STATE = -2, GIULIA_MHI_STAGE_DOMAIN = -3,
    GIULIA_MHI_STAGE_ALLOC = -4, GIULIA_MHI_STAGE_MAPPING = -5,
    GIULIA_MHI_STAGE_SYNC = -6, GIULIA_MHI_STAGE_BUSY = -7,
    GIULIA_MHI_STAGE_CLEANUP = -8, GIULIA_MHI_STAGE_PROVIDER = -9
};
struct giulia_mhi_stage_buffer {
    void *handle; uint8_t *cpu; uint64_t iova, bytes;
};
struct giulia_mhi_stage_ops {
    void *cookie;
    int (*pin)(void *, uint32_t sid, uint64_t first, uint64_t last, void **owner);
    int (*live)(void *, void *owner);
    int (*owns)(void *, void *owner, const struct giulia_mhi_stage_buffer *);
    int (*alloc)(void *, void *owner, uint32_t bytes, uint32_t align,
        struct giulia_mhi_stage_buffer *out);
    int (*sync)(void *, void *owner, const struct giulia_mhi_stage_buffer *, uint32_t used);
    int (*free)(void *, void *owner, void *handle);
    int (*quiesced)(void *, void *owner);
    int (*unpin)(void *, void *owner);
};
/* All callbacks return zero for success (including live/owns/quiesced).
 * pin obtains a unique non-NULL owner token that pins the same domain,
 * generation, endpoint and routing for this transfer. Its lifetime extends
 * beyond prepare, across submission and through last free; no token recycling
 * or concurrent revoke/rebind/check-then-use window. On detected loss the
 * provider denies operations until it can attest to that exact owner again.
 * ops are copied, cookie/callbacks/owner must outlive all retries. Caller
 * serializes ALL callbacks and API entry with provider reset, revoke and
 * doorbell activity. No reentrancy, including from callbacks.
 *
 * alloc transfers one unique opaque lease with writable CPU mapping and IOVA
 * of its coherent backing in this owner. Padding must be pre-cleared by the
 * provider. owns proves the ENTIRE descriptor (handle, CPU range, IOVA,
 * extent and domain), not just handle membership. No physical-page aliases,
 * including distinct virtual aliases or aliases of input/provider metadata;
 * caller/provider guarantee these, which numeric comparisons cannot detect.
 * sync publishes prior CPU writes to the device in call order; success of
 * the final table sync is the publication barrier for all earlier writes.
 * Neither alloc nor sync submits DMA. free releases exactly the one lease;
 * unpin releases the owner. Failed free/unpin MUST leave ownership entirely
 * unchanged and retryable (never fail after freeing). Failed pin/alloc
 * supplies a zeroed output and transfers no ownership. Null/duplicate handles or
 * ambiguous transfers violate this contract and can force permanent retention.
 * Successful quiesced irrevocably retires this owner's ONE transfer: no
 * reuse/re-arm or further DMA until owner release (including cleanup retries).
 * It is a device-side guarantee covering
 * outstanding DMA and posted writes or a proven cancellation before submit;
 * loss of link, mere timeout or absence of a callback is not that proof. */

struct giulia_mhi_stage_view {
    uint64_t bhi_iova, bhie_iova;
    uint32_t bhi_bytes, bhie_bytes; /* BHI prefix; BHIE vector table, NOT image length */
};
struct giulia_mhi_stage_slot {
    struct giulia_mhi_stage_buffer buffer;
    uint32_t used;
};
/* Caller-owned zero-initialized durable storage (not kernel stack). Do not
 * copy, move, reinitialize or edit after prepare starts. phase/count are
 * inspectable; other members are implementation-private. Sticky POISONED
 * requires external owner reconciliation; never zero this object to recover. */
struct giulia_mhi_stage_state {
    enum giulia_mhi_stage_phase phase;
    uint32_t count;
    void *owner;
    struct giulia_mhi_stage_ops ops;
    struct giulia_mhi_fw_plan plan;
    struct giulia_mhi_stage_slot slots[GIULIA_MHI_STAGE_SLOTS];
    struct giulia_mhi_fw_mapping vectors[GIULIA_MHI_FW_MAX_SEGMENTS];
};
/* image, state and ops use valid, distinct CPU objects; image is an opaque
 * immutable firmware file, accessible for all of prepare. prepare copies
 * separate BHI prefix plus the entire BHIE image into coherent leases;
 * it does not open a file, enable BME, issue MMIO, or submit a transfer.
 * Failures unwind in reverse order if provably safe. If state != EMPTY,
 * retain it and inspect/retry release; only EMPTY is reusable.
 * submit publishes both sets of addresses to a distinct caller-owned view,
 * changing READY to FLIGHT BEFORE returning addresses; no hardware is touched.
 * Caller must not access these buffers via state or cache stale views. Release
 * after submit requires quiescence even if the caller never rang a doorbell. */
int giulia_mhi_stage_prepare(struct giulia_mhi_stage_state *,
    const struct giulia_mhi_stage_ops *, const uint8_t *image, uint64_t bytes);
int giulia_mhi_stage_submit(struct giulia_mhi_stage_state *, struct giulia_mhi_stage_view *);
int giulia_mhi_stage_release(struct giulia_mhi_stage_state *);
#endif
