/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_MHI_FIRMWARE_H
#define GIULIA_MHI_FIRMWARE_H
#include <stdint.h>

/* WCN7850/KIWI in normal PBL, non-TME full boot-chain download. CNSS2 sets
 * both SBL and segment lengths to 512 KiB. A TX vector is two 64-bit words. */
#define GIULIA_MHI_FW_SEGMENT_BYTES 0x80000u
#define GIULIA_MHI_FW_ENTRY_BYTES 16u
/* Driver resource limit, NOT a firmware-format or silicon limit. */
#define GIULIA_MHI_FW_MAX_SEGMENTS 64u
#define GIULIA_MHI_FW_MAX_BYTES (GIULIA_MHI_FW_MAX_SEGMENTS * GIULIA_MHI_FW_SEGMENT_BYTES)

enum giulia_mhi_fw_result {
    GIULIA_MHI_FW_OK = 0, GIULIA_MHI_FW_ARGUMENT = -1,
    GIULIA_MHI_FW_SIZE = -2, GIULIA_MHI_FW_BOUNDS = -3,
    GIULIA_MHI_FW_OVERLAP = -4
};
struct giulia_mhi_fw_plan {
    uint64_t image_bytes;
    uint32_t bhi_bytes, data_segments, table_bytes;
};
/* A caller-owned, ALREADY mapped contiguous device-visible range. addr is an
 * IOVA, not a CPU pointer; bytes is the extent of the mapping, not the number
 * of firmware bytes actually used from it. */
struct giulia_mhi_fw_mapping { uint64_t addr, bytes; };

/* No I/O, allocation or firmware parsing. BHI consumes the bounded prefix;
 * BHIE consumes the entire image starting at byte zero, including that prefix.
 * All outputs are unchanged on error. */
int giulia_mhi_fw_plan(uint64_t image_bytes, struct giulia_mhi_fw_plan *out);
int giulia_mhi_fw_segment(uint64_t image_bytes, uint32_t index,
    uint64_t *offset, uint32_t *bytes);

/* Emit little-endian {IOVA:u64, actual_data_bytes:u64} pairs into CPU memory.
 * The table itself is a separate mapping, NOT a data entry. The inclusive
 * aperture must contain the entire declared span of every mapping. Disjoint
 * mappings may be noncontiguous or out of order; data[] is in file order.
 * As a driver admission policy data IOVAs must be 4-byte aligned and the table
 * IOVA 8-byte aligned. Zero is valid only if the caller mapped it. No assumption
 * on the alignment of the CPU output pointer; excess capacity is untouched.
 *
 * Caller serializes exclusive, stable inputs and supplies distinct, valid CPU
 * buffers (no output/input alias; segment output scalars must also be distinct).
 * Checking numeric spans is not proof of a mapping or device ownership. The
 * caller is responsible for IOMMU lifetime, backing data, table publication,
 * cache coherence and any later transfer. No register, DMA or BME operation. */
int giulia_mhi_fw_build_table(uint64_t image_bytes,
    const struct giulia_mhi_fw_mapping *data, uint32_t data_count,
    const struct giulia_mhi_fw_mapping *table, uint64_t aperture_first,
    uint64_t aperture_last, uint8_t *out, uint32_t out_capacity);
#endif
