/* SPDX-License-Identifier: BSD-2-Clause */
/* Independent TX layout for KIWI. Protocol references:
 * OnePlusOSS android_kernel_modules_and_devicetree_oneplus_sm8650 453d8875,
 * cnss2/pci.c: cnss_pci_register_mhi (SBL/segment sizes, full boot chain);
 * OnePlusOSS android_kernel_oneplus_sm8650 66305c56, drivers/bus/mhi/host/
 * boot.c mhi_fw_load_handler, mhi_firmware_copy; internal.h bhi_vec_entry. */
#include "giulia_mhi_firmware.h"

static uint32_t segment_bytes(uint64_t image, uint64_t offset)
{
    uint64_t left = image - offset; /* offset was admitted by plan/index. */
    return left < GIULIA_MHI_FW_SEGMENT_BYTES ? (uint32_t)left : GIULIA_MHI_FW_SEGMENT_BYTES;
}

int giulia_mhi_fw_plan(uint64_t image_bytes, struct giulia_mhi_fw_plan *out)
{
    if (!out) return GIULIA_MHI_FW_ARGUMENT;
    if (!image_bytes || image_bytes > GIULIA_MHI_FW_MAX_BYTES) return GIULIA_MHI_FW_SIZE;
    uint32_t count = (uint32_t)((image_bytes - 1u) / GIULIA_MHI_FW_SEGMENT_BYTES) + 1u;
    *out = (struct giulia_mhi_fw_plan){ .image_bytes = image_bytes,
        .bhi_bytes = segment_bytes(image_bytes, 0), .data_segments = count,
        .table_bytes = count * GIULIA_MHI_FW_ENTRY_BYTES };
    return GIULIA_MHI_FW_OK;
}

int giulia_mhi_fw_segment(uint64_t image_bytes, uint32_t index,
    uint64_t *offset, uint32_t *bytes)
{
    struct giulia_mhi_fw_plan plan;
    if (!offset || !bytes) return GIULIA_MHI_FW_ARGUMENT;
    int rc = giulia_mhi_fw_plan(image_bytes, &plan);
    if (rc) return rc;
    if (index >= plan.data_segments) return GIULIA_MHI_FW_BOUNDS;
    uint64_t start = (uint64_t)index * GIULIA_MHI_FW_SEGMENT_BYTES;
    *offset = start;
    *bytes = segment_bytes(image_bytes, start);
    return GIULIA_MHI_FW_OK;
}

static int mapped(const struct giulia_mhi_fw_mapping *m, uint32_t need,
    uint32_t alignment, uint64_t first, uint64_t last)
{
    /* All callers require nonzero need. Use inclusive end arithmetic so a
     * range ending exactly at UINT64_MAX works without a wrapped sum. */
    return m->bytes >= need && !(m->addr & (alignment - 1u)) &&
        m->addr >= first && m->addr <= last && m->bytes - 1u <= last - m->addr;
}

static int overlaps(const struct giulia_mhi_fw_mapping *a,
    const struct giulia_mhi_fw_mapping *b)
{
    /* Compare gap with length, without constructing even a one-past-end IOVA. */
    return a->addr <= b->addr ? b->addr - a->addr < a->bytes : a->addr - b->addr < b->bytes;
}

static void le64(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0; i < 8u; i++) { p[i] = (uint8_t)v; v >>= 8; }
}

int giulia_mhi_fw_build_table(uint64_t image_bytes,
    const struct giulia_mhi_fw_mapping *data, uint32_t data_count,
    const struct giulia_mhi_fw_mapping *table, uint64_t aperture_first,
    uint64_t aperture_last, uint8_t *out, uint32_t out_capacity)
{
    struct giulia_mhi_fw_plan plan;
    if (!data || !table || !out || aperture_first > aperture_last) return GIULIA_MHI_FW_ARGUMENT;
    int rc = giulia_mhi_fw_plan(image_bytes, &plan);
    if (rc) return rc;
    if (data_count != plan.data_segments || out_capacity < plan.table_bytes ||
        !mapped(table, plan.table_bytes, 8u, aperture_first, aperture_last)) return GIULIA_MHI_FW_BOUNDS;

    /* Validate before exposing ANY entry, including on a bad final mapping. */
    for (uint32_t i = 0; i < data_count; i++) {
        uint32_t n = segment_bytes(image_bytes, (uint64_t)i * GIULIA_MHI_FW_SEGMENT_BYTES);
        if (!mapped(&data[i], n, 4u, aperture_first, aperture_last)) return GIULIA_MHI_FW_BOUNDS;
        if (overlaps(&data[i], table)) return GIULIA_MHI_FW_OVERLAP;
        for (uint32_t j = 0; j < i; j++)
            if (overlaps(&data[i], &data[j])) return GIULIA_MHI_FW_OVERLAP;
    }
    for (uint32_t i = 0; i < data_count; i++) {
        uint8_t *entry = out + i * GIULIA_MHI_FW_ENTRY_BYTES;
        le64(entry, data[i].addr);
        le64(entry + 8u, segment_bytes(image_bytes, (uint64_t)i * GIULIA_MHI_FW_SEGMENT_BYTES));
    }
    return GIULIA_MHI_FW_OK;
}
