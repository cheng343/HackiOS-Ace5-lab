/* SPDX-License-Identifier: BSD-2-Clause */
/* Original implementation using current-device protocol facts, with no
 * firmware code copied. The private/static evidence and profile hash are
 * documented in ACE5-WIFI-PORT.md. AP SRAM resource is the fixed vendor
 * pineapple.dtsi qcom,rpmh-stats-v4 reg=<0x0c3f0000 0x400>.
 * The CPU never accesses the protected ARC aperture in this path. */
#include "giulia_cx_vote.h"
int giulia_cx_vote_capture(struct giulia_aop *aop,const struct giulia_cx_vote_io *io,
    const struct giulia_arc_resource *cx,struct giulia_cx_vote *s)
{
    int rc;
    if (!s) return GIULIA_AOP_ARGUMENT;
    *s=(struct giulia_cx_vote){0};
    if (!aop || !io || !io->read32 || !cx) return GIULIA_AOP_ARGUMENT;
    if (io->phys!=GIULIA_CX_STATS_PHYS || io->size!=GIULIA_CX_STATS_SIZE ||
        cx->address!=0x30000u || !cx->count || cx->count>16)
        return GIULIA_CX_VOTE_LAYOUT;
    if ((rc=giulia_aop_send_text64(aop,"{class: misc_debug, res: cx_vote}"))) return rc;
    for (unsigned pass=0;pass<2;pass++) for (unsigned n=0;n<7;n++) {
        unsigned i=pass ? 6u-n:n;
        uint32_t value,offset=0x1f0u+i*4u;
        s->failed_offset=offset; s->reads++;
        s->callback_error=io->read32(io->cookie,offset,&value);
        if (s->callback_error) return GIULIA_CX_VOTE_IO;
        if (pass) s->after[i]=value; else s->words[i]=value;
    }
    for (unsigned i=0;i<7;i++) if (s->words[i]!=s->after[i]) return GIULIA_CX_VOTE_CHANGED;
    if (s->words[6]!=0x7f7f7f7fu) return GIULIA_CX_VOTE_VALUE;
    for (unsigned row=0;row<24;row++) {
        unsigned v=(s->words[row/4]>>((row%4)*8))&255u;
        s->selector[row]=(uint8_t)v;
        /* RV32 LUI 0x1810 followed by ADDI -2048 is 0x0180f800,
         * skipping rows11..15,23,24. Only rows0..23 are returned. */
        if ((0x0180f800u>>row)&1u) {
            if (v!=0xfeu) return GIULIA_CX_VOTE_VALUE;
        } else if (v>=cx->count) return GIULIA_CX_VOTE_VALUE;
    }
    s->valid=1;
    return 0;
}
