/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_CX_VOTE_H
#define GIULIA_CX_VOTE_H
#include "giulia_aop.h"
#include "giulia_arc.h"
#define GIULIA_CX_STATS_PHYS 0x0c3f0000u
#define GIULIA_CX_STATS_SIZE 0x400u
enum giulia_cx_vote_error { GIULIA_CX_VOTE_LAYOUT=-40, GIULIA_CX_VOTE_IO=-41,
    GIULIA_CX_VOTE_VALUE=-42, GIULIA_CX_VOTE_CHANGED=-43 };
struct giulia_cx_vote_io {
    void *cookie;
    int (*read32)(void *,uint32_t offset,uint32_t *);
    uint32_t phys,size;
};
struct giulia_cx_vote {
    uint32_t words[7],after[7],reads,valid,failed_offset;
    uint8_t selector[24]; /* Rows11/19/20 are the firmware's 0xfe markers. */
    int callback_error;
};
/* Current-device AOP firmware profile must be independently verified.
 * Its misc_debug/cx_vote handler runs synchronously before QMP ACK; it packs
 * rows0..23 and leaves word6=0x7f7f7f7f. Row24 is not returned.
 * Keep the real local QMP/loader-quiet owner, no competing debug-buffer user.
 * Sends exactly one fixed diagnostic message, no voltage/vote/rail command.
 * A timeout retains the actual AOP pending message/lease; never resend.
 * valid is an observation only, NOT a resource lease or restorable baseline. */
int giulia_cx_vote_capture(struct giulia_aop *,const struct giulia_cx_vote_io *,
    const struct giulia_arc_resource *cx,struct giulia_cx_vote *);
#endif
