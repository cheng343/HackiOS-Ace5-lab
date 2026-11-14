/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_AOP_H
#define GIULIA_AOP_H
#include <stdint.h>

/* Original polling AOSS/QMP transport. Protocol facts:
 * OnePlusOSS/android_kernel_oneplus_sm8650@66305c56af1fcd7299c906c3257e585be93d93ed
 * drivers/soc/qcom/qcom_aoss.c, drivers/mailbox/qcom-ipcc.c,
 * include/dt-bindings/mailbox/qcom-ipcc.h.
 * The latter uses IPCC SEND_ID +0xc, (client << 16) | signal; AOP/QMP = 0/0.
 * Pin the live board mapping separately; this core never maps or writes IPCC.
 */
#define GIULIA_AOP_MSGRAM_PHYS 0x0c300000u
#define GIULIA_AOP_MSGRAM_SIZE 0x400u
#define GIULIA_AOP_UP 0x0000ffffu
#define GIULIA_AOP_DOWN 0xffff0000u
enum giulia_aop_result {
    GIULIA_AOP_OK = 0, GIULIA_AOP_ARGUMENT = -1, GIULIA_AOP_STATE = -2,
    GIULIA_AOP_PREREQUISITE = -3, GIULIA_AOP_LAYOUT = -4,
    GIULIA_AOP_BUSY = -5, GIULIA_AOP_READBACK = -6,
    GIULIA_AOP_TIMEOUT = -7, GIULIA_AOP_DOORBELL = -8
};
enum giulia_aop_state { GIULIA_AOP_IDLE, GIULIA_AOP_OPEN,
    GIULIA_AOP_FAULT, GIULIA_AOP_STOPPED };
enum giulia_aop_condition {
    GIULIA_AOP_MMIO_READY = 1u << 0,
    GIULIA_AOP_LOCAL_CLIENTS_QUIESCED = 1u << 1,
    GIULIA_AOP_IPCC_OWNED = 1u << 2,
    /* Explicit lease of a fully open, empty inherited host channel.
     * Observing UP or an empty mailbox alone does not establish this lease. */
    GIULIA_AOP_INHERITED_CHANNEL_LEASE = 1u << 3
};
#define GIULIA_AOP_REQUIRED 7u
enum giulia_aop_policy { GIULIA_AOP_OPEN_COLD, GIULIA_AOP_BORROW_OPEN,
    GIULIA_AOP_OPEN_ON_INHERITED_LINK };
struct giulia_aop_io {
    void *cookie;
    uint32_t (*read32)(void *, uint32_t);
    void (*write32)(void *, uint32_t, uint32_t);
    void (*barrier)(void *);
    void (*delay_us)(void *, unsigned);
    int (*kick)(void *);
};
struct giulia_aop {
    struct giulia_aop_io io;
    uint32_t state, bytes, offset, size, borrowed, keep_link, owned, pending, polls;
    int error;
};
struct giulia_aop_snapshot { uint32_t words[15], message_length, changed; };
/* Only header words and length are read; payload is private and untouched.
 * A successful snapshot establishes no lease or firmware command completion. */
int giulia_aop_inspect(const struct giulia_aop_io *, uint32_t mapped_bytes,
    struct giulia_aop_snapshot *);
/* All accesses are 32-bit. Caller retains MMIO/IPCC and excludes every local
 * QMP client until close succeeds. One process-wide owner serializes objects.
 * Cold opening refuses any inherited local UP/partial state. BORROW_OPEN
 * requires the explicit lease and all eight protocol state/ack words UP.
 * OPEN_ON_INHERITED_LINK requires that same external lease, stable UP local
 * link/ACK and remote link/ACK, and a closed local channel/ACK. It opens only
 * the channel and preserves the original link on every cleanup path.
 * Neither policy adopts a nonempty mailbox. No radio/PDC command is sent.
 * A partial handshake keeps FAULT/ownership; close it before releasing maps.
 */
int giulia_aop_open(struct giulia_aop *, const struct giulia_aop_io *,
    uint32_t mapped_bytes, uint32_t conditions, unsigned policy);
/* len must be positive, a multiple of four and fit the current mailbox.
 * Success means the AOP protocol consumed the message length, not that a
 * regulator/table readback or any particular firmware command succeeded.
 * Timeout retains the pending message and owner. It does not clear length,
 * resend, undo firmware changes or restore a previous PDC table.
 */
int giulia_aop_send(struct giulia_aop *, const uint8_t *, uint32_t len);
int giulia_aop_send_text64(struct giulia_aop *, const char *);
/* Observe a late consumption only. Never retriggers a doorbell or a message. */
int giulia_aop_recover(struct giulia_aop *);
/* A borrowed channel is left UP and untouched. A channel opened by this
 * object is asked DOWN and its local ACKs are awaited (one second each).
 * A pending message must first be recovered. Failure retains the owner.
 * An inherited link stays UP when its newly opened local channel is closed.
 * Closing a QMP channel does not reverse messages already consumed by AOP.
 */
int giulia_aop_close(struct giulia_aop *);
#endif
