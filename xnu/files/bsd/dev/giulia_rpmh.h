/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_RPMH_H
#define GIULIA_RPMH_H
#include <stdint.h>

/* Ace 5 APPS RSC DRV2: live DT 0x17a20000/0x10000, TCS offset 0xd00,
 * ACTIVE TCS 0..2, no hardware channel. This core never maps MMIO itself.
 * CMD DB is live DT reserved memory 0x81c60000/0x20000, not a rail address.
 */
#define GIULIA_RPMH_MAX_COMMANDS 16u
#define GIULIA_RPMH_DB_MAX 0x20000u
enum giulia_rpmh_result {
    GIULIA_RPMH_OK = 0, GIULIA_RPMH_ARGUMENT = -1,
    GIULIA_RPMH_PREREQUISITE = -2, GIULIA_RPMH_BUSY = -3,
    GIULIA_RPMH_READBACK = -4, GIULIA_RPMH_TIMEOUT = -5,
    GIULIA_RPMH_STATE = -6, GIULIA_RPMH_DATABASE = -7,
    GIULIA_RPMH_NOT_FOUND = -8
};
enum giulia_rpmh_state {
    GIULIA_RPMH_IDLE = 0, GIULIA_RPMH_OWNED,
    GIULIA_RPMH_FAULT, GIULIA_RPMH_DONE
};
enum giulia_rpmh_busy_reason {
    GIULIA_RPMH_BUSY_NONE, GIULIA_RPMH_BUSY_OWNER,
    GIULIA_RPMH_BUSY_IRQ, GIULIA_RPMH_BUSY_SLOT
};
enum giulia_rpmh_prerequisite {
    GIULIA_RPMH_DRV2_MAPPED = 1u << 0,
    GIULIA_RPMH_V3_NONCHANNEL_LAYOUT = 1u << 1,
    GIULIA_RPMH_ALL_CLIENTS_GUARDED = 1u << 2,
    GIULIA_RPMH_ACTIVE_TCS_0_2_OWNED = 1u << 3,
    GIULIA_RPMH_POLL_IRQ_EXCLUSIVE = 1u << 4
};
#define GIULIA_RPMH_REQUIRED 31u
struct giulia_rpmh_io {
    void *cookie;
    uint32_t (*read32)(void *, uint32_t offset);
    void (*write32)(void *, uint32_t offset, uint32_t value);
    void (*barrier)(void *);
    void (*delay_us)(void *, unsigned);
};
struct giulia_rpmh_command { uint32_t address, data; };
struct giulia_rpmh {
    struct giulia_rpmh_io io;
    uint32_t state, prerequisites, count, dirty, triggered, completed;
    uint32_t saved_control, saved_wait, saved_irq_bit;
    uint32_t saved_cmd[GIULIA_RPMH_MAX_COMMANDS][3];
    uint32_t last_irq, last_tcs, last_command, polls;
    uint32_t read_request, response, response_valid;
    uint32_t slot;
    /* First admission failure from existing reads, before any RSC write.
     * Diagnostic only; these fields never relax idle/IRQ/ownership checks. */
    uint32_t busy_reason, busy_irq, busy_slot, busy_status, busy_enable, busy_control;
    int error, cleanup_error;
};

/* Process-wide, nonrecursive, nonblocking DRV2 lease. All RSC clients,
 * including legacy touch setup, MUST use it. It is not a PMIC resource lock
 * and cannot serialize another firmware master. The caller serializes use
 * of each object. A failed/in-flight transaction retains its owner token.
 * Do not release an owner merely because its thread exits or times out.
 */
int giulia_rpmh_bus_claim(const void *owner);
int giulia_rpmh_bus_release(const void *owner);
int giulia_rpmh_bus_owned(const void *owner);

/* No MMIO at init; missing prerequisite flags reject initialization.
 * POLL_IRQ_EXCLUSIVE means no ISR may acknowledge this DRV2 completion while
 * this lease is held (including legacy GIC routing). It is a caller proof.
 */
int giulia_rpmh_init(struct giulia_rpmh *, const struct giulia_rpmh_io *, uint32_t);
/* ACTIVE_ONLY writes, with response requested and ordered completion for
 * every command. All three active slots must be idle/unarmed; only slot 0 is
 * borrowed for WRITE. Saves/restores its programming and only its IRQ bit.
 * This DOES NOT save or undo prior remote VRM/BCM votes. DONE/completed prove
 * command completion, not a measured regulator voltage or working Wi-Fi.
 * At most 200 ms waiting per call, plus bounded posted-write readback polls.
 */
int giulia_rpmh_write(struct giulia_rpmh *, const struct giulia_rpmh_command *, unsigned);
/* One ACTIVE_ONLY resource READ, using the upstream RPMh read protocol:
 * WRITE bit clear, response requested, DATA untouched. Uses the same guarded
 * TCS/IRQ lease as write; it still programs and triggers an RSC transaction.
 * No remote resource vote is submitted. A stale RESP_DATA register alone is
 * never evidence: completion must belong to this newly issued request.
 * Timeout/response failure retains the transaction; recover does not resend.
 * No automatic Wi-Fi hook. Support on Ace 5 hardware remains unverified.
 * READ can borrow the first idle, ENABLE=0, TRIGGER=0 active slot, while all
 * three slots must be idle and have no pending completion. Other slots may
 * retain completed firmware command buffers/latched trigger bits; those
 * registers and IRQ enable bits are preserved, never cleared to get a slot.
 */
int giulia_rpmh_read(struct giulia_rpmh *, uint32_t address);
/* Pure extraction after successful read AND TCS cleanup. Output is untouched
 * on failure. Data is raw protocol output, not a physical voltage or a lease
 * on that resource; its per-DRV meaning still needs platform validation.
 * All-ones/drifting RESP_DATA is rejected by this backend's fault convention.
 */
int giulia_rpmh_read_result(const struct giulia_rpmh *, uint32_t *);
/* Resume an existing transaction/cleanup. Never retriggers it. Success means
 * the TCS lease is recovered; inspect completed to learn whether it issued.
 * On failure retain mapping, object and lease; reboot may be required.
 */
int giulia_rpmh_recover(struct giulia_rpmh *);

/* Resolve an actual named VRM base from the current immutable CMD DB mapping.
 * expected_version must be established by the platform; this repository has
 * no device CMD DB dump and does not invent its version or resource addresses.
 * The fixed public parser defines the header layout, but does not prescribe a
 * magic version value. Require exact expected_version, magic, bounded headers
 * and auxiliary data, one matching name, slave 4 and a 16-byte-aligned base.
 * CMD DB checksum is explicitly unused by the official implementation; this
 * is structural validation, not authentication or a prior-vote read API.
 */
int giulia_rpmh_resolve_vrm(const uint8_t *db, uint32_t bytes,
    uint32_t expected_version, const char *name, uint32_t *address);
#endif
