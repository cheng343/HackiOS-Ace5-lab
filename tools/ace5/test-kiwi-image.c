/* SPDX-License-Identifier: BSD-2-Clause */
/* Real KIWI register decoder + MHI snapshot + CPU image plan. No device,
 * private blob, DMA or MMIO write is needed. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "giulia_kiwi_image.h"

static uint32_t regs[0x400 / 4];
static uint8_t image[GIULIA_MHI_FW_SEGMENT_BYTES + 17];
static unsigned lookups;
static const char *expected_name;
enum file_mode { PRESENT, MISSING, REJECTED, EMPTY, OVERSIZE, WRAPS };
static enum file_mode mode;

static int read32(void *cookie, uint32_t off, uint32_t *v)
{
    assert(cookie == regs && off % 4 == 0 && off < sizeof(regs));
    *v = regs[off / 4];
    return 0;
}
static const uint8_t *lookup(const char *dir, const char *name, uint64_t *size)
{
    assert(!strcmp(dir, "kiwifw") && !strcmp(name, expected_name));
    lookups++;
    switch (mode) {
    case MISSING: *size=0; return NULL;
    case REJECTED: *size=UINT64_MAX; return NULL;
    case EMPTY: *size=0; return image;
    case OVERSIZE: *size=(uint64_t)GIULIA_MHI_FW_MAX_BYTES+1; return image;
    case WRAPS: *size=3; return (const uint8_t *)(uintptr_t)(UINTPTR_MAX-1);
    default: *size=sizeof(image); return image;
    }
}
static void capture(uint32_t raw, uint32_t ee, uint32_t ctrl, uint32_t status,
    struct giulia_kiwi_soc_info *soc, struct giulia_mhi_snapshot *m)
{
    const struct giulia_kiwi_bar_io bar={regs,read32,NULL,GIULIA_MHI_BAR_BYTES};
    const struct giulia_mhi_snapshot_io bus={regs,read32,GIULIA_MHI_BAR_BYTES};
    memset(regs,0,sizeof(regs));
    regs[0x00/4]=0x1000; regs[0x08/4]=0x01000000; regs[0x10/4]=0x02020820;
    regs[0x28/4]=0x100; regs[0x2c/4]=0x300;
    regs[0x38/4]=ctrl; regs[0x48/4]=status;
    regs[0x104/4]=1; regs[0x128/4]=ee;
    regs[GIULIA_KIWI_SOC_INFO_REG/4]=raw;
    assert(!giulia_kiwi_read_soc_info(&bar,soc));
    assert(!giulia_mhi_snapshot(&bus,GIULIA_MHI_SNAPSHOT_REQUIRED,m));
    assert(m->read_count==28);
}
static void no_view(const struct giulia_kiwi_image *out, enum giulia_kiwi_image_status st)
{
    assert(out->status==st && !out->cpu && !out->plan.image_bytes &&
        !out->plan.bhi_bytes && !out->plan.data_segments && !out->plan.table_bytes);
}
int main(void)
{
    struct giulia_kiwi_soc_info soc;
    struct giulia_mhi_snapshot m;
    struct giulia_kiwi_image out={0};
    image[0]=0x71; image[GIULIA_MHI_FW_SEGMENT_BYTES]=0x6e; image[sizeof(image)-1]=0x2f;
    const uint32_t ep=GIULIA_PCIE_LINK_EP_ID;
    for (unsigned i=0;i<2;i++) {
        expected_name=i?"amss.bin":"amss20.bin";
        capture(i?0x41109101u:0x41109201u,0,0,i?0:0x101,&soc,&m);
        mode=PRESENT;
        unsigned calls=lookups;
        giulia_kiwi_image_resolve(&out,ep,&soc,&m,lookup);
        assert(lookups==calls+1 && out.status==GIULIA_IMAGE_READY &&
            !strcmp(out.name,expected_name) && out.cpu==image);
        assert(out.plan.image_bytes==sizeof(image) &&
            out.plan.bhi_bytes==GIULIA_MHI_FW_SEGMENT_BYTES &&
            out.plan.data_segments==2 && out.plan.table_bytes==32);
        assert(out.cpu[0]==0x71 && out.cpu[out.plan.bhi_bytes]==0x6e &&
            out.cpu[out.plan.image_bytes-1]==0x2f);
        for (unsigned f=MISSING;f<=WRAPS;f++) {
            mode=(enum file_mode)f;
            calls=lookups;
            giulia_kiwi_image_resolve(&out,ep,&soc,&m,lookup);
            assert(lookups==calls+1 && !strcmp(out.name,expected_name));
            no_view(&out, f==MISSING?GIULIA_IMAGE_ABSENT:GIULIA_IMAGE_INVALID);
        }
    }
    unsigned baseline=lookups;
    giulia_kiwi_image_resolve(&out,ep,&soc,&m,NULL);
    no_view(&out,GIULIA_IMAGE_NOT_CHECKED);
    for (unsigned i=0;i<10;i++) {
        capture(i==0?0x41109301u:0x41109201u,i==1?1:i==2?2:0,
            i==3?GIULIA_MHI_CTRL_RESET:0,
            i==4?GIULIA_MHI_STATUS_SYSERR:i==5?0x200:0,&soc,&m);
        if (i==6) soc.major^=1;
        if (i==7) m.valid&=~GIULIA_MHI_SNAPSHOT_HAVE_AFTER;
        if (i==8) m.valid|=GIULIA_MHI_SNAPSHOT_CHANGED;
        giulia_kiwi_image_resolve(&out,i==9?0x110817cbu:ep,&soc,&m,lookup);
        no_view(&out,GIULIA_IMAGE_UNSUPPORTED);
        assert(lookups==baseline && !out.name);
    }
    puts("PASS: captured KIWI/PBL selects one bounded private CPU view, never a DMA image");
    return 0;
}
