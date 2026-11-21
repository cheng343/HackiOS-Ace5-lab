/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_MHI_PRIVATE_H
#define GIULIA_MHI_PRIVATE_H
#include "giulia_mhi_s1.h"
#include "giulia_mhi_wired.h"
#include "giulia_mhi_staging.h"

struct giulia_mhi_private_range {
    const struct giulia_mhi_wired *backing;
    uint64_t iova;  /* intended IOVA, not proof of a routing/ownership grant */
};
enum giulia_mhi_private_phase {
    GIULIA_MHI_PRIVATE_EMPTY, GIULIA_MHI_PRIVATE_BUILDING,
    GIULIA_MHI_PRIVATE_READY, GIULIA_MHI_PRIVATE_CLEANUP
};
struct giulia_mhi_private {
    const struct giulia_mhi_private *self;
    enum giulia_mhi_private_phase phase;
    uint32_t tables, tree_live, permit_pages;
    uint64_t permit_pa;
    struct giulia_mhi_s1 tree;
    struct giulia_mhi_wired store[GIULIA_MHI_S1_MAX_TABLES];
    struct giulia_mhi_s1_page desc[GIULIA_MHI_S1_MAX_TABLES];
};
/* Prepare/destroy a NEVER-INSTALLED root from independently owned wired
 * firmware TX buffers. The caller loans the complete allocations exclusively
 * to this object until destroy returns success: do not modify/release their
 * records, remap the physical pages, or submit them to DMA. The builder owns
 * its table pages; it does not free caller payloads. All inputs and state are
 * distinct, stable, serialized; state is zero-initialized and off kernel stack.
 * oas is a verified PA width (or test input), not the device's IOVA width.
 * Result 0 means the CPU-side tree is complete, NOT published to the SMMU.
 * No active stream/context, TTBR programming, TLBI, MSI, firmware submission
 * or startup hook. Neither READY nor a supplied IOVA implements staging pin().
 * On error retain object plus loans if phase != EMPTY and retry destroy. */
int giulia_mhi_private_build(struct giulia_mhi_private *,
    const struct giulia_mhi_private_range *, uint32_t count, uint32_t oas);
int giulia_mhi_private_destroy(struct giulia_mhi_private *);
#endif
