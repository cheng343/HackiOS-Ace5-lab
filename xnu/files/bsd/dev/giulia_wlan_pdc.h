/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_WLAN_PDC_H
#define GIULIA_WLAN_PDC_H
#include "giulia_aop.h"
#define GIULIA_WLAN_PDC_COUNT 10u
#define GIULIA_WLAN_PDC_ALL 0x3ffu
enum giulia_wlan_pdc_result {
    GIULIA_WLAN_PDC_BASELINE_UNKNOWN = -20, GIULIA_WLAN_PDC_TABLE_MISMATCH = -21,
    GIULIA_WLAN_PDC_RETAINED = 1
};
enum giulia_wlan_pdc_policy { GIULIA_WLAN_PDC_RESTORE_KNOWN,
    GIULIA_WLAN_PDC_KEEP_TABLE };
enum giulia_wlan_pdc_state { GIULIA_WLAN_PDC_IDLE, GIULIA_WLAN_PDC_DELIVERED,
    GIULIA_WLAN_PDC_FAULT, GIULIA_WLAN_PDC_STOPPED, GIULIA_WLAN_PDC_HELD };
enum giulia_wlan_pdc_condition {
    GIULIA_WLAN_PDC_EXCLUSIVE = 1u << 0,
    /* WLAN/BT, DMA and GPIO/SW_CTRL users are quiesced and their leases held. */
    GIULIA_WLAN_PDC_CONTROL_LINES_QUIET = 1u << 1
};
#define GIULIA_WLAN_PDC_REQUIRED 3u
struct giulia_wlan_pdc_baseline { uint32_t known_mask, millivolts[10]; };
struct giulia_wlan_pdc {
    struct giulia_aop *aop;
    struct giulia_wlan_pdc_baseline baseline;
    uint32_t state, policy, attempted, delivered, restoring, pending_index;
    int error, cleanup_error;
};
/* Target messages are fixed Ace5 stock-DT protocol strings, with zero padding
 * to 64 bytes. dt_table must exactly match the current device's NUL-separated
 * qcom,pdc_init_table, including its last NUL. This checks data, not ownership.
 * RESTORE_KNOWN needs a genuine prior firmware-table baseline for all entries:
 * an old DT, TX buffer or an ACK does not provide it. KEEP_TABLE is an explicit
 * forward-only policy, never selected by an automatic startup path.
 * AOP must be OPEN with its quiet/IPCC lease retained during every operation.
 * First error stops delivery; a timed-out message is never sent twice.
 */
int giulia_wlan_pdc_start(struct giulia_wlan_pdc *, struct giulia_aop *,
    const uint8_t *dt_table, uint32_t bytes,
    const struct giulia_wlan_pdc_baseline *, uint32_t conditions, unsigned policy);
/* Only observes the late ACK for the last pending message. If the transport
 * faulted before publishing, its owner must repair/reopen it first. */
int giulia_wlan_pdc_recover(struct giulia_wlan_pdc *);
/* Call after users/controls are again quiet. Known prior values compensate
 * every attempted command in reverse order. Partial restore retains owner
 * and progress for retry. KEEP_TABLE returns RETAINED and keeps table owner;
 * closing QMP does not erase the table. No applied-command readback exists. */
int giulia_wlan_pdc_stop(struct giulia_wlan_pdc *, uint32_t conditions);
#endif
