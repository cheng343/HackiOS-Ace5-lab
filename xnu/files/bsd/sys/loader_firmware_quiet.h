/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef LOADER_FIRMWARE_QUIET_H
#define LOADER_FIRMWARE_QUIET_H
#include <stdint.h>
#define LOADER_QUIET_MAGIC 0x3154454955515746ULL /* FWQUIET1 */
#define LOADER_QUIET_VERSION 1u
#define LOADER_QUIET_RESET_SECONDS 8u
#define LOADER_QUIET_RESET_ARMED 1u
#define LOADER_QUIET_RESET_FAILED 2u
/* Different writers use disjoint 128-byte cache lines. The loader initializes
 * both before CPU_ON, then only XNU writes request, only loader writes ack.
 * XNU maps normal noncached/WCOMB RAM; loader invalidates the request line and
 * cleans the ACK line. Never clean a cache line containing both writers.
 */
struct loader_firmware_quiet {
    uint64_t magic;
    uint32_t version, size;
    volatile uint32_t request;
    volatile uint32_t reset_request;
    uint32_t reserved_request[26];
    volatile uint32_t ack, active;
    uint32_t reset_capability;
    volatile uint32_t reset_ack;
    uint32_t reserved_ack[28];
} __attribute__((aligned(128)));
_Static_assert(sizeof(struct loader_firmware_quiet) == 256, "firmware quiet ABI size");
_Static_assert(__builtin_offsetof(struct loader_firmware_quiet, ack) == 128, "quiet writer isolation");
#endif
