/* SPDX-License-Identifier: BSD-2-Clause */
/* Finite, explicit HW DRV0 channel test. Both tables stay zero. The only
 * nonzero device write is CHN_UPDATE=1; no performance/BCM votes are enabled.
 * Dedicated PCIe0 domains must remain collapsed, the APPS firmware CPU is
 * parked with IRQs masked, and no other XNU hardware/client is admitted.
 */
#ifndef GIULIA_CRM_XNU_HOST_TEST
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <sys/hackios_platform.h>
#include <sys/loader_firmware_quiet.h>
#include <sys/systm.h>
#include <vm/memory_types.h>
#endif
#include "giulia_crm.h"
extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, unsigned);
extern void ml_io_unmap(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);
extern void loader_log_line(const char *);
struct gc_mapping { uintptr_t allocation,address; vm_size_t size; };
struct gc_trial {
    struct gc_mapping shared,gcc,crm;
    volatile struct loader_firmware_quiet *quiet;
    struct giulia_crm transport;
};
static struct gc_trial gc;
static unsigned gc_started;
static void gc_log(const char *line) { loader_log_line(line); printf("%s",line); }
static void gc_barrier(__unused void *cookie)
{
#ifndef GIULIA_CRM_XNU_HOST_TEST
    __asm__ volatile("dsb sy" ::: "memory");
#else
    __sync_synchronize();
#endif
}
static void gc_map(struct gc_mapping *m,uint64_t pa,vm_size_t bytes,unsigned flags)
{
    vm_offset_t page=trunc_page(pa),offset=pa-page;
    vm_size_t size=round_page(offset+bytes);
    m->allocation=ml_io_map_unmappable(page,size,flags);
    if (m->allocation) { m->address=m->allocation+offset; m->size=size; }
}
static void gc_unmap(struct gc_mapping *m)
{
    if (m->allocation) ml_io_unmap(m->allocation,m->size);
    *m=(struct gc_mapping){0};
}
static uint32_t gc_word(uintptr_t base,uint32_t off)
{
    uint32_t v=*(volatile uint32_t *)(base+off); gc_barrier(0); return v;
}
static void gc_delay(__unused void *cookie,unsigned us) { delay(us); }
static int gc_quiet_acquire(struct gc_trial *p)
{
    p->quiet->request=1; gc_barrier(0);
    for (unsigned i=0;i<10000;i++) {
        if (p->quiet->ack==1 && p->quiet->active==1) return 1;
        gc_delay(0,100);
    }
    p->quiet->request=0; gc_barrier(0); return 0;
}
static int gc_quiet_release(struct gc_trial *p)
{
    p->quiet->request=0; gc_barrier(0);
    for (unsigned i=0;i<10000;i++) {
        if (!p->quiet->ack && !p->quiet->active) return 1;
        gc_delay(0,100);
    }
    return 0;
}
static int gc_lease(void *cookie)
{
    struct gc_trial *p=cookie;
    if (!p->quiet || p->quiet->request!=1 || p->quiet->ack!=1 || p->quiet->active!=1)
        return -1;
    /* Header/TLMM snapshots are not a lease. With the firmware CPU parked
     * and single-client XNU, exclude any live PCIe0 user independently:
     * both GDSC SW_COLLAPSE=1, HW control off, down-complete and not up,
     * collapse votes withdrawn, and all functional PCIe0 leaf votes off.
     * The inherited RCHNG/reference/NoC votes are neither taken nor changed.
     * CESTA may execute autonomously; no stop/direct-GCC ownership is claimed.
     * HW DRV0 is the fixed vendor PCIe0 client; other HW/SW DRVs are untouched.
     */
    uint32_t core=gc_word(p->gcc.address,0x6b004),phy=gc_word(p->gcc.address,0x6c000);
    uint32_t cstatus=gc_word(p->gcc.address,0x6b008),pstatus=gc_word(p->gcc.address,0x6c004);
    uint32_t collapse=gc_word(p->gcc.address,0x5214c),leaves=gc_word(p->gcc.address,0x52008);
    if (core==UINT32_MAX || phy==UINT32_MAX || cstatus==UINT32_MAX ||
        pstatus==UINT32_MAX || collapse==UINT32_MAX || leaves==UINT32_MAX) return -1;
    if ((core&7)!=1 || (phy&7)!=1 || (cstatus&0x18000)!=0x8000 ||
        (pstatus&0x18000)!=0x8000 || (collapse&9)!=9 || (leaves&0x3f)) return -1;
    return 0;
}
static int gc_read(void *cookie,uint32_t off,uint32_t *out)
{
    struct gc_trial *p=cookie;
    if (off>=0x3000 || (off&3)) return -1;
    *out=gc_word(p->crm.address,off); return *out==UINT32_MAX ? -1 : 0;
}
static int gc_write(void *cookie,uint32_t off,uint32_t value)
{
    struct gc_trial *p=cookie;
    int table=0;
    static const uint32_t perf[]={0,0x40,0x80,0x600,0x640};
    static const uint32_t bw[]={0xc0,0x140,0x680,0x700,0x780};
    /* Defense at the MMIO adapter as well as the core. This trial must not
     * introduce a vote or an undocumented UPDATE=0 rollback. */
    if (gc_lease(p) || off>=0x3000 || (off&3)) return -1;
    for (unsigned k=0;k<2;k++) for (unsigned st=0;st<5;st++)
        for (unsigned i=0;i<(k?p->transport.bw_count:p->transport.perf_count);i++)
            if (off==0x200+(k?bw[st]:perf[st])+4*i) table=1;
    if (off==0x1020 ? value!=1 : !table || value!=0) return -1;
    *(volatile uint32_t *)(p->crm.address+off)=value; gc_barrier(0); return 0;
}
static void gc_thread(void *context,__unused wait_result_t wait)
{
    uint64_t pa=(uint64_t)(uintptr_t)context;
    const struct giulia_crm_io io={&gc,gc_read,gc_write,gc_barrier,gc_delay,gc_lease};
    const struct giulia_crm_patch unchanged={GIULIA_CRM_PERF,0,0,7,0};
    int rc;
    gc_map(&gc.shared,pa,sizeof(struct loader_firmware_quiet),VM_WIMG_WCOMB);
    if (!gc.shared.address) goto done;
    gc.quiet=(volatile struct loader_firmware_quiet *)gc.shared.address;
    if (gc.quiet->magic!=LOADER_QUIET_MAGIC || gc.quiet->version!=LOADER_QUIET_VERSION ||
        gc.quiet->size!=sizeof(*gc.quiet) || gc.quiet->request || gc.quiet->ack || gc.quiet->active) {
        gc_log("ace5-crm: invalid/busy loader quiet service; no device writes\n"); goto done;
    }
    gc_map(&gc.gcc,0x100000,0x70000,VM_WIMG_IO);
    gc_map(&gc.crm,0x1d01000,0x3000,VM_WIMG_IO);
    if (!gc.gcc.address || !gc.crm.address) goto done;
    if (!gc_quiet_acquire(&gc)) {
        gc_log("ace5-crm: loader quiet ACK timeout; no device writes\n"); goto done;
    }
    rc=giulia_crm_start_empty_keep(&gc.transport,&io,&unchanged,1);
    {
        char line[180];
        snprintf(line,sizeof(line),"ace5-crm: status rc=%d busy=%#x update=%#x power-index=%#x\n",
            rc,gc_word(gc.crm.address,0x1000),gc_word(gc.crm.address,0x1020),gc_word(gc.crm.address,0x3f0));
        gc_log(line);
    }
    if (!rc) {
        gc_log("ace5-crm: empty channel0 acknowledged; all PERF/BW table words remain zero\n");
        rc=giulia_crm_stop(&gc.transport);
        if (rc==GIULIA_CRM_RETAINED)
            gc_log("ace5-crm: channel test complete; selected channel and firmware quiet retained until kernel return\n");
    } else {
        char line[160];
        snprintf(line,sizeof(line),"ace5-crm: channel test rc=%d published=%u pending=%u state=%u\n",
            rc,gc.transport.published,gc.transport.pending,gc.transport.state); gc_log(line);
    }
    if (gc.transport.owned) {
        /* Once any request may have issued, keep mappings, objects and quiet
         * through kernel return. No attempt to clear the selected channel.
         * Even prep cleanup faults retain their prerequisites for diagnosis. */
        if (gc.transport.published) {
            gc_log("ace5-crm: retain resources; no select-none or power rollback attempted\n"); goto retain;
        }
        if (giulia_crm_stop(&gc.transport)) goto retain;
    }
    if (!gc_quiet_release(&gc)) goto retain;
done:
    gc_unmap(&gc.crm); gc_unmap(&gc.gcc); gc_unmap(&gc.shared);
retain:
    thread_terminate(current_thread());
}
void giulia_crm_test_init(void)
{
    uint32_t enabled=0,cpus=0,cpu=0,seconds=0;
    DTEntry entry; const void *property; unsigned size; uint64_t pa; thread_t thread;
    if (!PE_parse_boot_argn("ace5_crm_channel_test",&enabled,sizeof(enabled)) || enabled!=1) return;
    if (hackios_platform()!=HACKIOS_PLATFORM_GIULIA) return;
    PE_parse_boot_argn("cpus",&cpus,sizeof(cpus)); PE_parse_boot_argn("bootcpu",&cpu,sizeof(cpu));
    PE_parse_boot_argn("loader_return_after",&seconds,sizeof(seconds));
    if (cpus!=1 || cpu!=1 || ml_get_topology_info()->num_cpus!=1 || seconds<10 || seconds>120) {
        gc_log("ace5-crm: requires cpus=1 bootcpu=1 and finite 10..120 second run\n"); return;
    }
    const char *const incompatible[]={"ace5_wifi_probe","ace5_aop_channel_test","ace5_touch",
        "ace5_battery","ace5_cpu_khz","ace5_gpu_probe"};
    for (unsigned i=0;i<sizeof(incompatible)/sizeof(incompatible[0]);i++) {
        uint32_t value=0;
        if (PE_parse_boot_argn(incompatible[i],&value,sizeof(value)) && value) {
            gc_log("ace5-crm: another hardware client enabled; no test started\n"); return;
        }
    }
    if (SecureDTLookupEntry(NULL,"/chosen",&entry)!=kSuccess ||
        SecureDTGetProperty(entry,"loader-firmware-quiet",&property,&size)!=kSuccess || size!=sizeof(pa)) {
        gc_log("ace5-crm: loader has no firmware quiet service; no device writes\n"); return;
    }
    __builtin_memcpy(&pa,property,sizeof(pa)); if (!pa || (pa&127u)) return;
    if (__atomic_exchange_n(&gc_started,1,__ATOMIC_ACQ_REL)) return;
    if (kernel_thread_start(gc_thread,(void *)(uintptr_t)pa,&thread)==KERN_SUCCESS) thread_deallocate(thread);
    else gc_log("ace5-crm: no test thread\n");
}
