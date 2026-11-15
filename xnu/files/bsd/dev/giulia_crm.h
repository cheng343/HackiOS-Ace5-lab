/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_CRM_H
#define GIULIA_CRM_H
#include <stdint.h>
#define GIULIA_CRM_STATES 5u
#define GIULIA_CRM_MAX_PERF 8u
#define GIULIA_CRM_MAX_BW 16u
#define GIULIA_CRM_PERF 0u
#define GIULIA_CRM_BW 1u
#define GIULIA_CRM_BW_FIELDS 0x6fffffffu

enum giulia_crm_result {
    /* Channel remains selected; no return-to-uninitialized state claimed. */
    GIULIA_CRM_RETAINED = 1,
    GIULIA_CRM_ARGUMENT = -1, GIULIA_CRM_STATE = -2,
    GIULIA_CRM_LEASE = -3, GIULIA_CRM_LAYOUT = -4,
    GIULIA_CRM_BUSY = -5, GIULIA_CRM_IO = -6,
    GIULIA_CRM_DRIFT = -7, GIULIA_CRM_TIMEOUT = -8
};
enum giulia_crm_state {
    GIULIA_CRM_IDLE, GIULIA_CRM_PREPARING, GIULIA_CRM_ACTIVE,
    GIULIA_CRM_FAULT, GIULIA_CRM_STOPPED, GIULIA_CRM_HELD
};
struct giulia_crm_io {
    void *cookie;
    int (*read32)(void *, uint32_t offset, uint32_t *);
    int (*write32)(void *, uint32_t offset, uint32_t);
    void (*barrier)(void *);
    void (*delay_us)(void *, unsigned);
    /* Real lease of PCIe CRM HW DRV0 tables, channel requests and MMIO.
     * All other local/firmware writers must remain excluded, and callers
     * must quiesce dependent WLAN/PCIe/DMA users before start AND stop.
     * Autonomous CESTA power-state execution need not be stopped by this
     * transport, but its sequence must accept this channel-table update.
     * Return zero only with this platform contract actually held. */
    int (*validate_lease)(void *);
};
struct giulia_crm_patch {
    uint32_t kind, state, resource, mask, value;
};
struct giulia_crm_table {
    uint32_t perf[5][8], bw[5][16];
};
struct giulia_crm {
    struct giulia_crm_io io;
    struct giulia_crm_table saved[2];
    uint32_t header[4], perf_count, bw_count;
    uint32_t state, original, target, dirty, pending, pending_channel, owned;
    uint32_t empty, published;
    int error, cleanup_error, callback_error;
};
/* Original implementation of the fixed OnePlus SM8650 CRM dual-channel
 * protocol (crm.c@66305c56af1fcd7299c906c3257e585be93d93ed), HW DRV0 only.
 * Caller verifies current live-DT base=0x1d01000,size=0x3000 and compatible
 * qcom,pcie-crm. Zero-initialize and keep object's address stable until stop.
 * Both UPDATE and BUSY must identify the same one-hot settled channel; an
 * uninitialized/ambiguous channel is rejected, never guessed as channel0.
 * Clone all states/resources of the active table into the inactive table,
 * apply disjoint masked patches, read back every word, then request exactly
 * one switch. The original active table is never written. Unrelated nodes
 * (including IP0), states and reserved bits are preserved.
 * This is table transport, NOT a NoC-ready/CESTA-handoff or voltage proof.
 * BCM scaling, current CMD DB aux data, state policy and physical completion
 * checks belong to the platform. Nothing auto-starts or maps this driver.
 */
int giulia_crm_start(struct giulia_crm *, const struct giulia_crm_io *,
    const struct giulia_crm_patch *, unsigned patch_count);
/* Explicit forward-only first initialization. Re-read UPDATE=BUSY=0 and
 * ALL words of both declared tables=0 under the same real platform lease;
 * a prior snapshot or zero header alone is insufficient. Stage channel0,
 * then request it once, following vendor crm_get_channel's initial choice.
 * Does not infer that CESTA is stopped from an empty table or START=0.
 * Before any channel request, failed preparation may restore the zero table.
 * After a request may have issued, stop/recover must retain owner, tables,
 * mappings and platform lease. There is no verified "select no channel"
 * command: UPDATE=0 is NEVER used as a fabricated rollback. Stop then returns
 * RETAINED only after ACK, or a fault/timeout while still unconfirmed.
 * Caller must arrange an independent controlled reset/reinitialization
 * before releasing this lifetime. This path is NEVER an automatic fallback
 * from giulia_crm_start and never used by the read-only probe.
 */
int giulia_crm_start_empty_keep(struct giulia_crm *, const struct giulia_crm_io *,
    const struct giulia_crm_patch *, unsigned patch_count);
/* Only polls an already requested switch, never resends it. A timeout keeps
 * both mappings, snapshots and owner. Later ACK can be observed here. */
int giulia_crm_recover(struct giulia_crm *);
/* Quiesce users first and retain the same lease. Switch back to the untouched
 * original channel and observe completion before restoring the formerly
 * inactive table. An incomplete rollback retains ownership for a retry.
 * Success restores table/channel state, not applied physical regulator state.
 * The empty-start API follows the explicit retained-lifetime contract above.
 */
int giulia_crm_stop(struct giulia_crm *);
#endif
