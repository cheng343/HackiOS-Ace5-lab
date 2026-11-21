/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_KIWI_IMAGE_H
#define GIULIA_KIWI_IMAGE_H
#include "giulia_kiwi_reg.h"
#include "giulia_mhi_firmware.h"
#include "giulia_mhi_snapshot.h"
#include "giulia_pcie_link.h"

/* A borrowed immutable RAM-image CPU view, not authenticated firmware,
 * wired staging or a DMA lease. All operations are serialized; view must
 * not outlive the backing RAM disk or the owning bringup session. */
typedef const uint8_t *(*giulia_kiwi_lookup_fn)(const char *dir, const char *name, uint64_t *size);
enum giulia_kiwi_image_status {
    GIULIA_IMAGE_NOT_CHECKED, GIULIA_IMAGE_UNSUPPORTED, GIULIA_IMAGE_ABSENT,
    GIULIA_IMAGE_INVALID, GIULIA_IMAGE_READY
};
struct giulia_kiwi_image {
    enum giulia_kiwi_image_status status;
    const char *name;
    const uint8_t *cpu;
    struct giulia_mhi_fw_plan plan;
};
static inline const char *giulia_kiwi_image_status_name(enum giulia_kiwi_image_status st)
{
    switch (st) {
    case GIULIA_IMAGE_NOT_CHECKED: return "not-checked";
    case GIULIA_IMAGE_UNSUPPORTED: return "unsupported";
    case GIULIA_IMAGE_ABSENT: return "absent";
    case GIULIA_IMAGE_INVALID: return "invalid";
    case GIULIA_IMAGE_READY: return "image-ready";
    }
    return "invalid";
}
/* Call only after the actual BAR identity + complete snapshot succeeded.
 * PCIe root PCI ID is verified elsewhere; the endpoint PCI ID must be KIWI.
 * Strict preparatory policy: normal PBL (EE=0), MHI RESET or READY. Never
 * load an image for a running SBL/mission, EDL, RDDM or ambiguous sample.
 * This eligibility is NOT authorization to access a live device later. */
static inline void giulia_kiwi_image_resolve(struct giulia_kiwi_image *out,
    uint32_t endpoint_id, const struct giulia_kiwi_soc_info *soc,
    const struct giulia_mhi_snapshot *m, giulia_kiwi_lookup_fn lookup)
{
    const uint32_t all = GIULIA_MHI_SNAPSHOT_HAVE_HEADER | GIULIA_MHI_SNAPSHOT_HAVE_BHI |
        GIULIA_MHI_SNAPSHOT_HAVE_BHIE | GIULIA_MHI_SNAPSHOT_HAVE_AFTER;
    *out = (struct giulia_kiwi_image){.status=GIULIA_IMAGE_UNSUPPORTED};
    if (endpoint_id!=GIULIA_PCIE_LINK_EP_ID || !soc || !soc->raw || soc->raw==UINT32_MAX ||
        soc->major!=(uint8_t)(soc->raw>>8) || soc->minor!=(uint8_t)soc->raw ||
        soc->device!=((soc->raw>>16)&0xfffu) || soc->family!=(soc->raw>>28) ||
        ((soc->major&15u)!=1u && (soc->major&15u)!=2u) || !m || m->valid!=all ||
        m->bhi[GIULIA_MHI_BHI_EXECENV] || m->bhi_after[GIULIA_MHI_BHI_EXECENV]) return;
    for (unsigned i=0;i<2;i++) {
        const uint32_t *h=i?m->header_after:m->header;
        if ((h[GIULIA_MHI_CONTROL]&GIULIA_MHI_CTRL_RESET) ||
            (h[GIULIA_MHI_STATUS]&GIULIA_MHI_STATUS_SYSERR) ||
            (h[GIULIA_MHI_STATUS]&GIULIA_MHI_STATE_MASK)>0x100u) return;
    }
    out->name=giulia_kiwi_fw_name(soc);
    if (!lookup) { out->status=GIULIA_IMAGE_NOT_CHECKED; return; }
    uint64_t n=0;
    const uint8_t *p=lookup("kiwifw",out->name,&n);
    if (!p) { out->status=n?GIULIA_IMAGE_INVALID:GIULIA_IMAGE_ABSENT; return; }
    if (!n || n>GIULIA_MHI_FW_MAX_BYTES || n-1u>UINTPTR_MAX-(uintptr_t)p ||
        giulia_mhi_fw_plan(n,&out->plan)) { out->status=GIULIA_IMAGE_INVALID; return; }
    out->cpu=p; out->status=GIULIA_IMAGE_READY;
}
#endif
