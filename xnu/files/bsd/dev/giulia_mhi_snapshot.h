/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_MHI_SNAPSHOT_H
#define GIULIA_MHI_SNAPSHOT_H
#include <stdint.h>

#define GIULIA_MHI_BAR_BYTES 0x200000u
#define GIULIA_MHI_HEADER_WORDS 7u
#define GIULIA_MHI_BHI_WORDS 5u
#define GIULIA_MHI_BHIE_WORDS 2u
#define GIULIA_MHI_STATE_MASK 0xff00u
#define GIULIA_MHI_CTRL_RESET (1u << 1)
#define GIULIA_MHI_STATUS_SYSERR (1u << 2)
#define GIULIA_MHI_STATUS_READY 1u
#define GIULIA_MHI_TRANSFER_STATUS_MASK 0xc0000000u

enum giulia_mhi_snapshot_result {
    GIULIA_MHI_SNAPSHOT_OK = 0, GIULIA_MHI_SNAPSHOT_ARGUMENT = -1,
    GIULIA_MHI_SNAPSHOT_PREREQUISITE = -2,
    GIULIA_MHI_SNAPSHOT_READ_ERROR = -3,
    GIULIA_MHI_SNAPSHOT_LAYOUT = -4,
    GIULIA_MHI_SNAPSHOT_UNAVAILABLE = -5
};
enum giulia_mhi_snapshot_prerequisite {
    GIULIA_MHI_SNAPSHOT_WCN7850_BAR0 = 1u << 0,
    GIULIA_MHI_SNAPSHOT_CPU_MEM_READY = 1u << 1,
    GIULIA_MHI_SNAPSHOT_MMIO_READABLE = 1u << 2,
    GIULIA_MHI_SNAPSHOT_USERS_QUIESCED = 1u << 3
};
#define GIULIA_MHI_SNAPSHOT_REQUIRED 15u
enum giulia_mhi_snapshot_valid {
    GIULIA_MHI_SNAPSHOT_HAVE_HEADER = 1u << 0,
    GIULIA_MHI_SNAPSHOT_HAVE_BHI = 1u << 1,
    GIULIA_MHI_SNAPSHOT_HAVE_BHIE = 1u << 2,
    GIULIA_MHI_SNAPSHOT_HAVE_AFTER = 1u << 3,
    GIULIA_MHI_SNAPSHOT_CHANGED = 1u << 4
};
enum giulia_mhi_snapshot_header_index {
    GIULIA_MHI_REGLEN, GIULIA_MHI_VERSION, GIULIA_MHI_CONFIG,
    GIULIA_MHI_BHI_OFFSET, GIULIA_MHI_BHIE_OFFSET,
    GIULIA_MHI_CONTROL, GIULIA_MHI_STATUS
};
enum giulia_mhi_snapshot_bhi_index {
    GIULIA_MHI_BHI_VERSION_MINOR, GIULIA_MHI_BHI_VERSION_MAJOR,
    GIULIA_MHI_BHI_EXECENV, GIULIA_MHI_BHI_STATUS, GIULIA_MHI_BHI_ERROR
};
enum giulia_mhi_snapshot_bhie_index {
    GIULIA_MHI_BHIE_TX_STATUS, GIULIA_MHI_BHIE_RX_STATUS
};
struct giulia_mhi_snapshot_io {
    void *cookie;
    /* Ordered device read from the already mapped physical BAR0 + offset;
     * zero means success. May check link accessibility but must not wake the
     * endpoint, change register windows, write doorbells or change votes. */
    int (*read32)(void *, uint32_t offset, uint32_t *value);
    uint32_t bar_bytes;
};
struct giulia_mhi_snapshot {
    uint32_t valid, read_count, failed_offset;
    int callback_error;
    uint32_t header[GIULIA_MHI_HEADER_WORDS];
    uint32_t bhi[GIULIA_MHI_BHI_WORDS];
    uint32_t bhie[GIULIA_MHI_BHIE_WORDS];
    uint32_t header_after[GIULIA_MHI_HEADER_WORDS];
    uint32_t bhi_after[GIULIA_MHI_BHI_WORDS];
    uint32_t bhie_after[GIULIA_MHI_BHIE_WORDS];
};
/* One bounded read-only observation (at most 28 reads), not MHI startup.
 * Caller owns/serializes the link and has established CPU BAR decoding with
 * BME/IRQ/DMA users quiesced, PERST# released and endpoint MMIO accessible.
 * CPU decoding alone does not establish accessibility during low power.
 * There are no writes, allocation, polling, firmware transfer or DMA setup.
 * Offset fields are full u32: reject rather than mask/truncate bad addresses.
 * Snapshot validity is per complete section. CHANGED detects observed changes
 * only, not ABA; lack of CHANGED never establishes an atomic snapshot/lease.
 * BHIE RX registers have undefined power-on values in Linux's documented
 * initialization: raw status/sequence words, including all ones, are not proof
 * of an earlier completed transfer. No serial, hardware ID or key hash read.
 */
int giulia_mhi_snapshot(const struct giulia_mhi_snapshot_io *, uint32_t,
    struct giulia_mhi_snapshot *);
#endif
