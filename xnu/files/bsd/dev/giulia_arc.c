/* SPDX-License-Identifier: BSD-2-Clause */
/* Independently implemented from public protocol facts in OnePlusOSS
 * android_kernel_oneplus_sm8650@66305c56af1fcd7299c906c3257e585be93d93ed:
 * drivers/soc/qcom/cmd-db.c (header/entry/aux layout),
 * drivers/regulator/rpmh-regulator.c (ARC LE16 levels, selectors, padding).
 * No Linux framework code or device CMD DB dump is embedded. */
#include "giulia_arc.h"
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0]|(uint16_t)p[1]<<8); }
static uint32_t u32(const uint8_t *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
int giulia_arc_resolve(const uint8_t *db,uint32_t bytes,uint32_t version,
    const char *name,struct giulia_arc_resource *out)
{
    struct giulia_arc_resource r={0};
    uint8_t key[8]={0}; unsigned len,ended=0,matches=0;
    if (!out) return GIULIA_RPMH_ARGUMENT;
    *out=r;
    if (!db || !name || !version || version==UINT32_MAX) return GIULIA_RPMH_ARGUMENT;
    for (len=0;len<8 && name[len];len++) key[len]=(uint8_t)name[len];
    if (!len || (len==8 && name[8])) return GIULIA_RPMH_ARGUMENT;
    if (bytes<144 || bytes>GIULIA_RPMH_DB_MAX || u32(db)!=version ||
        u32(db+4)!=0x0c0330dbu) return GIULIA_RPMH_DATABASE;
    for (unsigned s=0;s<8;s++) {
        const uint8_t *d=db+8+s*16;
        unsigned slave=u16(d),off=u16(d+2),aux=u16(d+4),count=u16(d+6);
        if (!slave) {
            if (count) return GIULIA_RPMH_DATABASE;
            ended=1; continue;
        }
        if (ended || slave>7 || count>512 || off>bytes-144 ||
            count*24u>bytes-144-off || aux>bytes-144) return GIULIA_RPMH_DATABASE;
        for (unsigned i=0;i<count;i++) {
            const uint8_t *e=db+144+off+i*24;
            unsigned n=u16(e+20),aoff=u16(e+22),same=1;
            if (aoff>bytes-144-aux || n>bytes-144-aux-aoff) return GIULIA_RPMH_DATABASE;
            for (unsigned j=0;j<8;j++) if (e[j]!=key[j]) same=0;
            if (!same) continue;
            r.address=u32(e+16);
            if (++matches!=1 || slave!=3 || ((r.address>>16)&15u)!=3 ||
                (r.address&3u) || !n || n>32 || (n&1u)) return GIULIA_RPMH_DATABASE;
            const uint8_t *a=db+144+aux+aoff;
            unsigned padding=0;
            for (unsigned j=0;j<n/2;j++) {
                uint16_t level=u16(a+2*j);
                if (j && !level) padding=1;
                if (padding) {
                    if (level) return GIULIA_RPMH_DATABASE;
                    continue;
                }
                /* Max aggregation of selectors requires monotone levels. */
                if (j && level<=r.level[j-1]) return GIULIA_RPMH_DATABASE;
                r.level[r.count++]=level;
            }
        }
    }
    if (!matches) return GIULIA_RPMH_NOT_FOUND;
    *out=r; return 0;
}
int giulia_arc_select(const struct giulia_arc_resource *r,uint32_t minimum,uint32_t *selector)
{
    if (!selector) return GIULIA_RPMH_ARGUMENT;
    *selector=UINT32_MAX;
    if (!r || !r->count || r->count>GIULIA_ARC_MAX_LEVELS) return GIULIA_RPMH_ARGUMENT;
    for (unsigned i=1;i<r->count;i++)
        if (r->level[i]<=r->level[i-1]) return GIULIA_RPMH_DATABASE;
    for (unsigned i=0;i<r->count;i++) if (r->level[i]>=minimum) {
        *selector=i; return 0;
    }
    return GIULIA_RPMH_NOT_FOUND;
}
