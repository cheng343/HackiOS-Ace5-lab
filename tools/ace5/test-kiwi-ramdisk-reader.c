/* SPDX-License-Identifier: BSD-2-Clause
 * Host-only translation unit: exercise the production reader and production APFS
 * parser. The substitutes are only kernel allocation, locking and RAM-disk I/O. */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "apfs_lib.h"
#include "giulia_kiwi_image.h"

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "failed at %s:%d: %s\n", \
    __FILE__, __LINE__, #expr); exit(2); } } while (0)
typedef uintptr_t vm_offset_t;
typedef size_t vm_size_t;
#define Z_ZERO 1
#define Z_WAITOK 2
#define LCK_GRP_DECLARE(var, label) int var __attribute__((unused))
#define LCK_MTX_DECLARE(var, grp) pthread_mutex_t var = PTHREAD_MUTEX_INITIALIZER
#define lck_mtx_lock(m) ((void)pthread_mutex_lock(m))
#define lck_mtx_unlock(m) ((void)pthread_mutex_unlock(m))
#define kalloc_type(type, flags) ((type *)calloc(1, sizeof(type)))
#define kfree_type(type, ptr) free(ptr)
#define __DECONST(type, ptr) ((type)(uintptr_t)(ptr))
static unsigned payload_allocs;
static void *payload_alloc(size_t n) { ++payload_allocs; return malloc(n); }
#define kalloc_data(n, flags) payload_alloc(n)
#define kfree_data(ptr, size) free(ptr)
static size_t shim_strlcpy(char *dst, const char *src, size_t cap)
{
    size_t n = strlen(src);
    if (cap) { size_t k = n < cap ? n : cap - 1; memcpy(dst, src, k); dst[k] = 0; }
    return n;
}
#define strlcpy shim_strlcpy
#define REDWOOD_IMAGE_HOST_TEST 1
#include "../../xnu/files/bsd/dev/redwood_image.c"

struct host_image { uint8_t *bytes; size_t size; int present, readable; };
static struct host_image disks[2];

static uint8_t *slurp(const char *path, size_t *bytes)
{
    FILE *f = fopen(path, "rb");
    CHECK(f && !fseek(f, 0, SEEK_END));
    long n = ftell(f);
    CHECK(n >= 0 && !fseek(f, 0, SEEK_SET));
    uint8_t *p = malloc(n ? (size_t)n : 1);
    CHECK(p && fread(p, 1, (size_t)n, f) == (size_t)n && !fclose(f));
    *bytes = (size_t)n;
    return p;
}
static void load_disk(unsigned i, const char *p)
{
    if (!strcmp(p, "@absent")) return;
    disks[i].present = 1;
    if (!strcmp(p, "@unreadable")) return;
    disks[i].bytes = slurp(p, &disks[i].size);
    disks[i].readable = 1;
}
dev_t mdevlookup(int disk)
{
    return disk >= 0 && disk <= 1 && disks[disk].present ? (dev_t)disk : (dev_t)-1;
}
int mdevgetkernelrange(dev_t dev, vm_offset_t *base, uint64_t *size)
{
    if (dev > 1 || !disks[dev].readable) return EIO;
    *base = (vm_offset_t)disks[dev].bytes;
    *size = disks[dev].size;
    return 0;
}
static int inside(const uint8_t *ptr, uint64_t len, const struct host_image *d)
{
    uintptr_t lo = (uintptr_t)d->bytes, at = (uintptr_t)ptr;
    return d->readable && at >= lo && at-lo <= d->size && len <= d->size-(at-lo);
}
static uint64_t lookup_cap;
static int lookup_count, resolver_disk;
static const uint8_t *lookup_real(const char *dir, const char *name, uint64_t *bytes)
{
    ++lookup_count;
    return redwood_image_boot_plain_file(dir, name, lookup_cap, bytes, &resolver_disk);
}
int main(int argc, char **argv)
{
    /* MODE MD0 MD1 NAME MAX (invalid|absent|EXPECTED_FILE) EXPECTED_DISK */
    CHECK(argc == 8);
    load_disk(0, argv[2]); load_disk(1, argv[3]);
    const char *name = argv[4], *result = argv[6];
    uint64_t bound = strtoull(argv[5], NULL, 10), size = 0x65432109ULL;
    int disk = -10, legacy = !strcmp(argv[1], "legacy-copy");
    CHECK(legacy || !strcmp(argv[1], "strict"));
    const uint8_t *p = legacy ? redwood_image_file("kiwifw", name, &size, &disk)
        : redwood_image_boot_plain_file("kiwifw", name, bound, &size, &disk);
    struct giulia_kiwi_image view = {0};
    if (!legacy) {
        /* Synthetic, self-consistent snapshots only; no BAR, registers or DMA. */
        uint8_t major = !strcmp(name, "amss20.bin") ? 0x22 : 0x21;
        struct giulia_kiwi_soc_info soc = {.raw = 0x12340005u | ((uint32_t)major << 8),
            .family=1, .device=0x234, .major=major, .minor=5};
        struct giulia_mhi_snapshot m = {.valid=GIULIA_MHI_SNAPSHOT_HAVE_HEADER |
            GIULIA_MHI_SNAPSHOT_HAVE_BHI | GIULIA_MHI_SNAPSHOT_HAVE_BHIE | GIULIA_MHI_SNAPSHOT_HAVE_AFTER};
        m.header[GIULIA_MHI_STATUS] = m.header_after[GIULIA_MHI_STATUS] = 0x101u;
        lookup_cap = bound;
        giulia_kiwi_image_resolve(&view, GIULIA_PCIE_LINK_EP_ID, &soc, &m, lookup_real);
        CHECK(lookup_count == 1 && view.name && !strcmp(name, view.name));
        CHECK(payload_allocs == 0); /* metadata allocations are permitted */
    }
    if (!strcmp(result, "invalid") || !strcmp(result, "absent")) {
        CHECK(!legacy && !p && size == (!strcmp(result, "invalid") ? UINT64_MAX : 0));
        CHECK(!view.cpu && !view.plan.image_bytes && view.status ==
            (!strcmp(result, "invalid") ? GIULIA_IMAGE_INVALID : GIULIA_IMAGE_ABSENT));
    } else {
        size_t bytes = 0;
        uint8_t *expected = slurp(result, &bytes);
        CHECK(p && size == bytes && disk == atoi(argv[7]) && (disk == 0 || disk == 1));
        CHECK(!memcmp(p, expected, bytes));
        if (legacy) {
            CHECK(payload_allocs == 1 && !inside(p,size,&disks[0]) && !inside(p,size,&disks[1]));
            CHECK(redwood_image_file("kiwifw", name, &size, &disk) == p && payload_allocs == 1);
            /* A generic decompression cached on this very disk is still ineligible. */
            CHECK(!redwood_image_boot_plain_file("kiwifw", name, bound, &size, &disk)
                && size == UINT64_MAX && payload_allocs == 1);
        } else {
            CHECK(inside(p, size, &disks[disk]) && !inside(p, size, &disks[1-disk]));
            CHECK(view.cpu == p && resolver_disk == disk && view.status == GIULIA_IMAGE_READY);
            uint32_t count = (uint32_t)((bytes+0x7ffffu) / 0x80000u);
            CHECK(view.plan.image_bytes == bytes && view.plan.data_segments == count &&
                  view.plan.table_bytes == 16u*count &&
                  view.plan.bhi_bytes == (bytes < 0x80000u ? bytes : 0x80000u));
            uint64_t covered = 0;
            for (uint32_t i = 0; i < count; ++i) {
                uint64_t off; uint32_t used;
                CHECK(!giulia_mhi_fw_segment(view.plan.image_bytes, i, &off, &used));
                CHECK(off == covered && off <= bytes && used && used <= bytes-off &&
                      !memcmp(view.cpu + off, expected + off, used));
                covered += used;
            }
            CHECK(covered == bytes);
        }
        free(expected);
    }
    /* Kept entries in the original generic reader intentionally live for the boot. */
    free(disks[0].bytes); free(disks[1].bytes);
    return 0;
}
