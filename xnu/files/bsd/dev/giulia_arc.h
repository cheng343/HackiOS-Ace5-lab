/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_ARC_H
#define GIULIA_ARC_H
#include "giulia_rpmh.h"
#define GIULIA_ARC_MAX_LEVELS 16u
struct giulia_arc_resource {
    uint32_t address, count;
    uint16_t level[GIULIA_ARC_MAX_LEVELS];
};
/* Current immutable CMD DB only: ARC slave3, bounded LE16 level table.
 * Consumer corner values are NOT the hardware selectors sent over RPMh.
 * No prior-vote read, guessed version/address or MMIO operation here. */
int giulia_arc_resolve(const uint8_t *,uint32_t bytes,uint32_t version,
    const char *name,struct giulia_arc_resource *);
int giulia_arc_select(const struct giulia_arc_resource *,uint32_t minimum,
    uint32_t *selector);
#endif
