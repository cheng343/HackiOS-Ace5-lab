/* SPDX-License-Identifier: BSD-2-Clause */
/* Independently implemented from register facts; no firmware code copied.
 * HLOS index2: OnePlusOSS SM8650@66305c56, sys_pm_vx.c drv_names_pineapple.
 * Current-device AOP static analysis establishes 25 ARC rows, 0x10000 stride,
 * 14 resources at 0x10 stride, low-four-bit selectors, local base0x600000.
 * The same firmware's address conversion establishes the AP +0xb000000 view.
 * AP accessibility is a separate, explicitly selected hardware diagnostic. */
#include "giulia_arc_votes.h"

int giulia_arc_votes_snapshot(const struct giulia_arc_votes_io *io,const uint8_t *db,
    uint32_t bytes,uint32_t version,struct giulia_arc_votes *s)
{
    static const char *const names[]={"xo.lvl","cx.lvl"};
    int rc;
    if (!s) return GIULIA_RPMH_ARGUMENT;
    *s=(struct giulia_arc_votes){0};
    if (!io || !io->read32) return GIULIA_RPMH_ARGUMENT;
    if (io->phys!=GIULIA_ARC_HLOS_PHYS || io->size!=GIULIA_ARC_HLOS_SIZE)
        return GIULIA_ARC_VOTES_LAYOUT;
    for (unsigned i=0;i<2;i++) {
        rc=giulia_arc_resolve(db,bytes,version,names[i],&s->resource[i]);
        if (rc) return rc;
        uint32_t address=s->resource[i].address;
        if (address<0x30000u || address>0x300d0u || (address&15u))
            return GIULIA_ARC_VOTES_LAYOUT;
    }
    if (s->resource[0].address==s->resource[1].address) return GIULIA_ARC_VOTES_LAYOUT;
    for (unsigned pass=0;pass<2;pass++) for (unsigned n=0;n<2;n++) {
        unsigned i=pass ? 1u-n:n;
        uint32_t offset=s->resource[i].address&0xffffu,value;
        s->reads++; s->failed_offset=offset;
        s->callback_error=io->read32(io->cookie,offset,&value);
        if (s->callback_error) return GIULIA_ARC_VOTES_IO;
        if (pass) s->after[i]=value; else s->raw[i]=value;
        if (value==UINT32_MAX || (value&15u)>=s->resource[i].count)
            return GIULIA_ARC_VOTES_VALUE;
        if (!pass) s->selector[i]=value&15u;
    }
    if (s->raw[0]!=s->after[0] || s->raw[1]!=s->after[1])
        return GIULIA_ARC_VOTES_CHANGED;
    s->valid=1;
    return 0;
}
