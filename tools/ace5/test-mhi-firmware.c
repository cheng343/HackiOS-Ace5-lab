/* SPDX-License-Identifier: BSD-2-Clause */
/* Pure caller-side tests. Link the production planner; do not start PCI/MHI. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "giulia_mhi_firmware.h"

#define CHUNK UINT32_C(524288)
#define LIMIT UINT64_C(33554432)
#define BASE UINT64_C(0x1234567800000000)
#define SLOTS 64u
#define VEC 16u
#define GUARD 13u
#define POISON 0xabu

static uint64_t decode_le64(const uint8_t p[8])
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= (uint64_t)p[i] << (8u * i);
    return value;
}

static void image(uint64_t bytes, uint32_t count, uint32_t last, uint32_t table_bytes)
{
    struct giulia_mhi_fw_plan plan;
    struct giulia_mhi_fw_mapping data[SLOTS];
    /* Distinct, nonmonotonic high IOVAs with gaps. No identity-mapped CPU pointers. */
    struct giulia_mhi_fw_mapping table = {BASE + UINT64_C(0x80123400), table_bytes};
    uint8_t box[GUARD + SLOTS*VEC + GUARD];
    memset(box, POISON, sizeof box);
    assert(!giulia_mhi_fw_plan(bytes, &plan));
    assert(plan.image_bytes == bytes && plan.data_segments == count);
    assert(plan.bhi_bytes == (bytes < CHUNK ? bytes : CHUNK) && plan.table_bytes == table_bytes);
    uint64_t consumed = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t off = UINT64_MAX;
        uint32_t size = UINT32_MAX;
        assert(!giulia_mhi_fw_segment(bytes, i, &off, &size));
        /* BHIe always includes offset 0, including BHI's prefix, without duplicating
         * it at the end or treating the descriptor table as an image segment. */
        assert(off == consumed && off == (uint64_t)CHUNK*i);
        assert(size == (i == count-1 ? last : CHUNK));
        if (i == 0) assert(off == 0 && size == plan.bhi_bytes);
        consumed += size;
        data[i].addr = BASE + (uint64_t)((i*23u + 7u) % SLOTS)*UINT64_C(0x201000) + UINT64_C(0x120);
        data[i].bytes = (uint64_t)size + 16u; /* descriptor must not include mapping slack */
    }
    assert(consumed == bytes);
    assert(!giulia_mhi_fw_build_table(bytes, data, count, &table, BASE, UINT64_MAX,
                                     box+GUARD, SLOTS*VEC));
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *entry = box + GUARD + VEC*i;
        uint32_t size = i == count-1 ? last : CHUNK;
        assert(decode_le64(entry) == data[i].addr);
        assert(decode_le64(entry+8) == size);
        assert(decode_le64(entry) != table.addr);
    }
    /* Exact length includes no self-entry. Preserve both unaligned-output guards. */
    for (size_t j=0; j<sizeof box; ++j)
        if (j<GUARD || j>=GUARD+table_bytes) assert(box[j] == POISON);
    uint64_t off = 12345;
    uint32_t size = 6789;
    assert(giulia_mhi_fw_segment(bytes, count, &off, &size) < 0);
    assert(off == 12345 && size == 6789);
}

static void reject_image(uint64_t bytes)
{
    struct giulia_mhi_fw_plan guard, saved;
    uint64_t off = 123;
    uint32_t size = 456;
    memset(&guard, 0xa5, sizeof guard);
    memcpy(&saved, &guard, sizeof saved);
    assert(giulia_mhi_fw_plan(bytes, &guard) < 0);
    assert(memcmp(&guard, &saved, sizeof guard) == 0);
    assert(giulia_mhi_fw_segment(bytes, 0, &off, &size) < 0);
    assert(off == 123 && size == 456);
}

static void reject_table(uint64_t bytes, const struct giulia_mhi_fw_mapping *data, uint32_t count,
                         const struct giulia_mhi_fw_mapping *table, uint64_t lo, uint64_t hi, uint32_t cap)
{
    uint8_t out[96], before[96];
    memset(out, 0x5a, sizeof out);
    memcpy(before, out, sizeof out);
    assert(giulia_mhi_fw_build_table(bytes, data, count, table, lo, hi, out+8, cap) < 0);
    assert(memcmp(out, before, sizeof out) == 0);
}

/* Error at entry 64 must not leak entries 1..63 into the caller's DMA buffer.
 * A hole is legal; a segment straddling the supplied inclusive aperture is not. */
static void late_invalid_mapping(void)
{
    struct giulia_mhi_fw_mapping d[SLOTS], t = {BASE, SLOTS*VEC};
    uint8_t box[2*GUARD+SLOTS*VEC], before[sizeof box];
    for (unsigned i=0;i<SLOTS;i++)
        d[i]=(struct giulia_mhi_fw_mapping){BASE+(1u+i)*UINT64_C(0x200000), CHUNK};
    const uint64_t end = d[SLOTS-1].addr+CHUNK-1u;
    memset(box, POISON, sizeof box);
    memcpy(before, box, sizeof box);
    assert(giulia_mhi_fw_build_table(LIMIT,d,SLOTS,&t,BASE,end-1,box+GUARD,SLOTS*VEC)
           == GIULIA_MHI_FW_BOUNDS);
    assert(memcmp(before,box,sizeof box)==0);
    d[SLOTS-1].addr=d[0].addr+CHUNK-4u;
    assert(giulia_mhi_fw_build_table(LIMIT,d,SLOTS,&t,BASE,end,box+GUARD,SLOTS*VEC)
           == GIULIA_MHI_FW_OVERLAP);
    assert(memcmp(before,box,sizeof box)==0);
}

static void windows_and_errors(void)
{
    const uint64_t bytes = CHUNK+1u;
    const struct giulia_mhi_fw_mapping original[] = {{0, CHUNK}, {CHUNK, 1}};
    struct giulia_mhi_fw_mapping d[2], t = {CHUNK+8u, 32};
    uint8_t out[32];
    uint64_t hi=t.addr+t.bytes-1;
    memcpy(d, original, sizeof d);
    /* Adjoining intervals & zero IOVA are legal; aperture last is inclusive. */
    assert(!giulia_mhi_fw_build_table(bytes, d, 2, &t, 0, hi, out, sizeof out));
    assert(decode_le64(out) == 0 && decode_le64(out+16) == CHUNK);
    /* Valid mapped table at the very end of the 64-bit address space. */
    struct giulia_mhi_fw_mapping end = {UINT64_MAX-31,32};
    assert(!giulia_mhi_fw_build_table(bytes, d, 2, &end, 0, UINT64_MAX, out, sizeof out));
    reject_table(0,d,2,&t,0,hi,32);
    reject_table(LIMIT+1,d,2,&t,0,hi,32);
    reject_table(UINT64_MAX,d,2,&t,0,hi,32);
    reject_table(bytes,d,1,&t,0,hi,32);
    reject_table(bytes,d,UINT32_MAX,&t,0,hi,32);
    reject_table(bytes,NULL,2,&t,0,hi,32);
    reject_table(bytes,d,2,NULL,0,hi,32);
    reject_table(bytes,d,2,&t,0,hi,31);
    reject_table(bytes,d,2,&t,1,hi,32);
    reject_table(bytes,d,2,&t,hi,0,32);
    reject_table(bytes,d,2,&t,0,hi-1,32);
    struct giulia_mhi_fw_mapping bad = {t.addr,31};
    reject_table(bytes,d,2,&bad,0,hi,32);
    bad = (struct giulia_mhi_fw_mapping){t.addr+4,32};
    reject_table(bytes,d,2,&bad,0,UINT64_MAX,32);
    bad = (struct giulia_mhi_fw_mapping){UINT64_MAX-7,32};
    reject_table(bytes,d,2,&bad,0,UINT64_MAX,32);
    bad = (struct giulia_mhi_fw_mapping){0,32};
    reject_table(bytes,d,2,&bad,0,hi,32);  /* table aliases first payload */
    bad = (struct giulia_mhi_fw_mapping){CHUNK,32};
    reject_table(bytes,d,2,&bad,0,hi,32);  /* table aliases last payload */
    d[1].addr=CHUNK-4u;
    reject_table(bytes,d,2,&t,0,hi,32);    /* data-data alias in final entry */
    d[1]=original[1]; d[1].bytes=0;
    reject_table(bytes,d,2,&t,0,hi,32);
    d[1]=original[1]; d[1].addr+=1;
    reject_table(bytes,d,2,&t,0,hi,32);
    d[1]=(struct giulia_mhi_fw_mapping){UINT64_MAX-3,5};
    reject_table(bytes,d,2,&t,0,UINT64_MAX,32);  /* full mapped span wraps */
    d[1]=original[1]; d[1].bytes=9;
    reject_table(bytes,d,2,&t,0,hi,32);    /* slack intersects table, not payload bytes */
    d[1]=original[1]; d[0].bytes--;
    reject_table(bytes,d,2,&t,0,hi,32);
    d[0]=original[0];
    assert(giulia_mhi_fw_build_table(bytes,d,2,&t,0,hi,NULL,32)<0);
    assert(giulia_mhi_fw_plan(bytes,NULL)<0);
    uint64_t off=123;
    uint32_t size=456;
    assert(giulia_mhi_fw_segment(bytes,0,NULL,&size)<0 && size==456);
    assert(giulia_mhi_fw_segment(bytes,0,&off,NULL)<0 && off==123);
    assert(giulia_mhi_fw_segment(bytes,UINT32_MAX,&off,&size)<0 && off==123 && size==456);
}

int main(void)
{
    assert(GIULIA_MHI_FW_SEGMENT_BYTES==CHUNK && GIULIA_MHI_FW_MAX_SEGMENTS==SLOTS);
    assert(GIULIA_MHI_FW_ENTRY_BYTES==VEC && GIULIA_MHI_FW_MAX_BYTES==LIMIT);
    image(6352960,13,61504,208);     /* observed KIWI image A */
    image(7458880,15,118848,240);   /* observed KIWI image B */
    image(1,1,1,16);
    image(CHUNK-1,1,CHUNK-1,16);
    image(CHUNK,1,CHUNK,16);
    image(CHUNK+1,2,1,32);
    image(2*CHUNK,2,CHUNK,32);
    image(LIMIT-1,64,CHUNK-1,1024);
    image(LIMIT,64,CHUNK,1024);
    reject_image(0); reject_image(LIMIT+1); reject_image(UINT64_MAX);
    late_invalid_mapping();
    windows_and_errors();
    puts("MHI firmware planner/vector wire fixtures passed (offline, no endpoint I/O)");
    return 0;
}
