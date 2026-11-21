/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_MHI_WIRED_H
#define GIULIA_MHI_WIRED_H
#include <stdint.h>

/* Detached CPU backing only. No IOVA, device mapping or DMA authorization.
 * Each object owns entire native VM pages, never shared with another object.
 * An I/O-table leaf is 4 KiB even if the native VM page is 16 KiB. */
#define GIULIA_MHI_WIRED_SLICE 4096u
#define GIULIA_MHI_WIRED_MAX   0x80000u /* one KIWI data segment */
struct giulia_mhi_wired {
    const struct giulia_mhi_wired *self;
    uintptr_t base; /* original VM entry incl. both CPU guards; private */
    uint32_t bytes, extent, page;
};
/* Use zeroed durable storage; never move, copy, edit or concurrently operate
 * on a live object. Zero denotes success. Outputs unchanged on rejection
 * (caller supplies distinct writable outputs). Alloc/free may sleep. */
int giulia_mhi_wired_alloc(struct giulia_mhi_wired *, uint32_t bytes);
int giulia_mhi_wired_cpu(const struct giulia_mhi_wired *, void **cpu, uint32_t *bytes);
/* Whole 4 KiB slices covering payload, including zero tail padding.
 * Returned address is CPU PHYSICAL: never publish as a KIWI DMA address.
 * Its aperture/width, translation and permissions are the mapper's job. */
int giulia_mhi_wired_phys4k(const struct giulia_mhi_wired *, uint32_t offset, uint64_t *pa);
/* CPU-to-device cache clean to PoC; no DMA. Empty ranges rejected. */
int giulia_mhi_wired_clean(const struct giulia_mhi_wired *, uint32_t offset, uint32_t bytes);
/* Caller MUST have retired all users, device mappings, DMA and walk-cache
 * references beforehand. This layer cannot attest device quiescence; never
 * use timeout or release itself as evidence. Empty release is idempotent. */
int giulia_mhi_wired_release(struct giulia_mhi_wired *);
#endif
