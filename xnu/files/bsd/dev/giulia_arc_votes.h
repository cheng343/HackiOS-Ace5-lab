/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_ARC_VOTES_H
#define GIULIA_ARC_VOTES_H
#include "giulia_arc.h"

/* SM8650 ARC HLOS row, not the VRM voter numbering or the APPS RSC window.
 * See docs/ACE5-WIFI-PORT.md for this fixed profile's evidence and limits. */
#define GIULIA_ARC_HLOS_PHYS 0x0b620000u
#define GIULIA_ARC_HLOS_SIZE 0x10000u
enum giulia_arc_votes_result {
    GIULIA_ARC_VOTES_LAYOUT=-30, GIULIA_ARC_VOTES_IO=-31,
    GIULIA_ARC_VOTES_VALUE=-32, GIULIA_ARC_VOTES_CHANGED=-33
};
struct giulia_arc_votes_io {
    void *cookie;
    int (*read32)(void *,uint32_t offset,uint32_t *);
    uint32_t phys,size;
};
struct giulia_arc_votes {
    struct giulia_arc_resource resource[2]; /* XO, CX */
    uint32_t raw[2],after[2],selector[2],reads,valid,failed_offset;
    int callback_error;
};
/* Diagnostic only: resolve both resources before table reads, sample each
 * twice and validate the selector against that boot's CMD DB. No write API.
 * Caller must independently verify the firmware/profile and AP access before
 * mapping. A stable observation is NOT a retained ownership lease, a voltage
 * measurement, a sleep/wake vote or permission to restore a shared resource. */
int giulia_arc_votes_snapshot(const struct giulia_arc_votes_io *,const uint8_t *,
    uint32_t bytes,uint32_t version,struct giulia_arc_votes *);
#endif
