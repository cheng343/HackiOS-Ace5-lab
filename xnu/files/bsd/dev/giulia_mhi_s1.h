/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_MHI_S1_H
#define GIULIA_MHI_S1_H
#include <stdint.h>

/* Offline, NEVER attached 32-bit IAS, 4 KiB AArch64 stage-1 page tables
 * for PKG110's KIWI TX pool (excludes MSI, IPA). Not a live DMA domain. */
#define GIULIA_MHI_S1_FIRST 0x98000000ULL
#define GIULIA_MHI_S1_LAST  0xafffffffULL
#define GIULIA_MHI_S1_PAGE 4096u
#define GIULIA_MHI_S1_LEAVES 192u /* one page of 512 entries per 2 MiB */
#define GIULIA_MHI_S1_MAX_TABLES (GIULIA_MHI_S1_LEAVES + 2u)
enum giulia_mhi_s1_result {
    GIULIA_MHI_S1_OK=0, GIULIA_MHI_S1_ARGUMENT=-1, GIULIA_MHI_S1_STATE=-2,
    GIULIA_MHI_S1_OWNER=-3, GIULIA_MHI_S1_RANGE=-4, GIULIA_MHI_S1_CONFLICT=-5,
    GIULIA_MHI_S1_CAPACITY=-6
};
struct giulia_mhi_s1_page { uint8_t *cpu; uint64_t pa; };
struct giulia_mhi_s1_ops {
    void *cookie;
    int (*owns_table)(void *, const struct giulia_mhi_s1_page *);
    int (*owns_tx)(void *, uint64_t first_pa, uint32_t pages);
};
/* All storage, ops and calls are serialized. The caller pins all pages and
 * grants the builder exclusive use of these initially ZERO, 4K-aligned table
 * pages. owns_table must verify backing CPU<->PA association, writable extent,
 * and private, never-installed ownership. owns_tx must verify the entire
 * physical run really backs pinned device-readable firmware TX RAM and is
 * independent of the builder's metadata. Neither can just check a number,
 * and a CPU pointer must never be substituted for hardware physical backing.
 * Zero callback return is proof; failure leaves state/table bytes unchanged.
 * No revocation/generation change or caller writes between these checks and
 * any operation; pins outlive mappings, including if cleanup is refused.
 * Caller must prevent hidden virtual/physical aliases; builder checks the
 * supplied spans and previously mapped addresses. Ops/cookie remain valid.
 *
 * Use durable, zero-initialized, non-stack state; never copy/move or change
 * its fields after successful init. Do not install its root while builder
 * owns it. Buffers/metadata are distinct and no callback may re-enter.
 * No OS allocation, descriptor publication/clean, TLB invalidation, MMIO,
 * stream ownership or live attach. Reset is NOT safe on an installed root. */
struct giulia_mhi_s1 {
    struct giulia_mhi_s1_ops ops;
    uint64_t pa_last;
    uint32_t page_count, used;
    uint8_t leaf[GIULIA_MHI_S1_LEAVES]; /* physical table pool index; 0 absent */
    struct giulia_mhi_s1_page table[GIULIA_MHI_S1_MAX_TABLES];
};
/* First two pages are root / intermediate, following pages are sparse leaf
 * storage. 3..194 pages total. OAS is a caller-confirmed PA width, never
 * inferred from the TBU *input* width. Accept 32/36/40/42/44/48. */
int giulia_mhi_s1_init(struct giulia_mhi_s1 *, const struct giulia_mhi_s1_ops *,
    const struct giulia_mhi_s1_page *, uint32_t count, uint32_t oas_bits);
/* Map/unmap a 4K-aligned contiguous physical run transactionally. Multiple
 * calls support non-contiguous backing. Distinct IOVAs may NOT alias the
 * same physical page. Unmap requires exact original PA(s), never overwrites
 * foreign mappings. Sparse leaf-table pages remain reserved until reset. */
int giulia_mhi_s1_map(struct giulia_mhi_s1 *, uint64_t iova, uint64_t pa, uint32_t pages);
int giulia_mhi_s1_unmap(struct giulia_mhi_s1 *, uint64_t iova, uint64_t pa, uint32_t pages);
int giulia_mhi_s1_reset(struct giulia_mhi_s1 *);
#endif
