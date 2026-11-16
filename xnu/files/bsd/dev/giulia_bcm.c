/* SPDX-License-Identifier: BSD-2-Clause */
/* Original bounded decoder/ICC-to-CRM implementation. Protocol facts from
 * OnePlusOSS/android_kernel_oneplus_sm8650@66305c56af1fcd7299c906c3257e585be93d93ed:
 * drivers/soc/qcom/cmd-db.c; drivers/interconnect/qcom/{icc-rpmh.[ch],
 * bcm-voter.c,pineapple.c}; include/soc/qcom/{cmd-db.h,tcs.h}.
 * Raw firmware/DT/SDK data is not embedded here. No Linux source copied.
 */
#include "giulia_bcm.h"
#include "giulia_pcie_profile.h"
static uint32_t le16(const uint8_t *p) { return p[0]|(uint32_t)p[1]<<8; }
static uint32_t le32(const uint8_t *p) { return le16(p)|le16(p+2)<<16; }
int giulia_bcm_resolve(const uint8_t *db,uint32_t bytes,uint32_t version,
    const char *name,struct giulia_bcm_resource *out)
{
    uint8_t key[8]={0}; unsigned len,ended=0,found=0;
    struct giulia_bcm_resource value={0};
    if (!db || !name || !out || !version || version==UINT32_MAX) return GIULIA_BCM_ARGUMENT;
    *out=value;
    for (len=0;len<8 && name[len];len++) key[len]=(uint8_t)name[len];
    if (!len || (len==8 && name[8])) return GIULIA_BCM_ARGUMENT;
    if (bytes<144 || bytes>0x20000 || le32(db)!=version || le32(db+4)!=0x0c0330db)
        return GIULIA_BCM_DATABASE;
    for (unsigned s=0;s<8;s++) {
        const uint8_t *d=db+8+s*16;
        uint32_t slave=le16(d),off=le16(d+2),aux=le16(d+4),count=le16(d+6);
        if (!slave) { if (count) return GIULIA_BCM_DATABASE; ended=1; continue; }
        if (ended || slave>7 || count>512 || off>bytes-144 ||
            count*24>bytes-144-off || aux>bytes-144) return GIULIA_BCM_DATABASE;
        for (unsigned i=0;i<count;i++) {
            const uint8_t *e=db+144+off+i*24;
            uint32_t alen=le16(e+20),aoff=le16(e+22),same=1;
            if (aoff>bytes-144-aux || alen>bytes-144-aux-aoff) return GIULIA_BCM_DATABASE;
            for (unsigned j=0;j<8;j++) if (e[j]!=key[j]) same=0;
            if (!same) continue;
            uint32_t address=le32(e+16);
            if (++found!=1 || slave!=5 || ((address>>16)&15u)!=5 || (address&3u) || alen<8)
                return GIULIA_BCM_DATABASE;
            const uint8_t *a=db+144+aux+aoff;
            value.address=address; value.unit=le32(a); value.width=le16(a+4); value.vcd=a[6];
            /* Vendor qcom_icc_bcm_init normalizes these two zero fields. */
            if (!value.unit) value.unit=1;
            if (!value.width) value.width=1;
        }
    }
    if (!found) return GIULIA_BCM_NOT_FOUND;
    *out=value; return 0;
}
static uint64_t divide(uint64_t n,uint32_t d) { return n && n<d ? 1 : n/d; }
static int numeric(uint64_t bw,const struct giulia_bcm_resource *r,uint32_t divisor,uint64_t *v)
{
    if (bw>UINT64_MAX/r->width) return GIULIA_BCM_OVERFLOW;
    uint64_t scaled=divide(bw*r->width,divisor);
    if (scaled>UINT64_MAX/1000) return GIULIA_BCM_OVERFLOW;
    *v=divide(scaled*1000,r->unit); return 0;
}
int giulia_bcm_encode(const struct giulia_bcm_resource *r,uint64_t avg,uint64_t peak,
    uint32_t buswidth,uint32_t channels,uint32_t enable,struct giulia_bcm_vote *out)
{
    uint64_t x=0,y=0; int rc;
    if (!r || !out || !r->unit || !r->width || !buswidth || !channels ||
        channels>UINT32_MAX/buswidth || enable>1) return GIULIA_BCM_ARGUMENT;
    *out=(struct giulia_bcm_vote){0};
    if (enable) y=!!(avg || peak);
    else if ((rc=numeric(avg,r,buswidth*channels,&x)) ||
        (rc=numeric(peak,r,buswidth,&y))) return rc;
    out->saturated=x>0x3fff || y>0x3fff;
    out->x=x>0x3fff ? 0x3fff : (uint32_t)x;
    out->y=y>0x3fff ? 0x3fff : (uint32_t)y;
    out->command=0x60000000u|(out->x<<14)|out->y;
    return 0;
}
int giulia_bcm_pcie0_plan(const uint8_t *db,uint32_t bytes,uint32_t version,
    unsigned policy,struct giulia_bcm_pcie0_plan *out)
{
    static const char *const names[5]={"MC0","SH0","SH1","SN4","ACV"};
    static const uint32_t node[5]={0,1,2,3,5},width[5]={4,16,16,16,4},channels[5]={4,4,1,1,4};
    struct giulia_bcm_pcie0_plan plan={0}; int rc;
    if (!out || policy>GIULIA_BCM_PCIE0_GEN1_X1_250000_0) return GIULIA_BCM_ARGUMENT;
    *out=plan;
    uint64_t avg=policy==GIULIA_BCM_PCIE0_FIXED_500_800 ? 500 : 250000;
    uint64_t peak=policy==GIULIA_BCM_PCIE0_FIXED_500_800 ? 800 : 0;
    plan.patch[0]=(struct giulia_crm_patch){0,0,0,7,GIULIA_PCIE_BOOT_PERF};
    plan.patch[1]=(struct giulia_crm_patch){0,1,0,7,GIULIA_PCIE_BOOT_PERF};
    for (unsigned i=0;i<5;i++) {
        if ((rc=giulia_bcm_resolve(db,bytes,version,names[i],&plan.resource[i])) ||
            (rc=giulia_bcm_encode(&plan.resource[i],avg,peak,width[i],channels[i],i==2 || i==4,&plan.vote[i])))
            return rc;
        for (unsigned j=0;j<i;j++)
            if (plan.resource[i].address==plan.resource[j].address) return GIULIA_BCM_DATABASE;
        for (unsigned st=0;st<2;st++)
            /* Generic BCM TCS encoding has COMMIT. CRM HW DRV tables expose
             * only X/Y+VALID; fixed vendor crm.c marks bit30 SW-only. */
            plan.patch[2+2*i+st]=(struct giulia_crm_patch){1,st,node[i],GIULIA_CRM_BW_FIELDS,
                plan.vote[i].command & GIULIA_CRM_BW_FIELDS};
    }
    *out=plan; return 0;
}
int giulia_bcm_pcie0_start(struct giulia_crm *p,const struct giulia_crm_io *io,
    const uint8_t *db,uint32_t bytes,uint32_t version,unsigned policy,struct giulia_bcm_pcie0_plan *plan)
{
    int rc=giulia_bcm_pcie0_plan(db,bytes,version,policy,plan);
    return rc ? rc : giulia_crm_start(p,io,plan->patch,GIULIA_BCM_PCIE0_PATCHES);
}
int giulia_bcm_pcie0_start_empty_keep(struct giulia_crm *p,const struct giulia_crm_io *io,
    const uint8_t *db,uint32_t bytes,uint32_t version,unsigned policy,struct giulia_bcm_pcie0_plan *plan)
{
    int rc=giulia_bcm_pcie0_plan(db,bytes,version,policy,plan);
    return rc ? rc : giulia_crm_start_empty_keep(p,io,plan->patch,GIULIA_BCM_PCIE0_PATCHES);
}
