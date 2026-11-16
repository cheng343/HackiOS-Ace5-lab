/* SPDX-License-Identifier: BSD-2-Clause */
/* Independent finite XNU adapter for the public RPMh READ wire protocol.
 * Pinned pineapple.dtsi: DRV2=17a20000, IRQ=SPI5 (INTID37), GICD=17100000.
 * GIC register facts: Linux v6.12 include/linux/irqchip/arm-gic-v3.h and
 * drivers/irqchip/irq-gic-v3.c (single-bit W1S/W1C mask and RWP completion).
 * No ARC aperture, resource WRITE, GPIO or PCIe access.
 */
#ifndef GIULIA_RPMH_PROBE_HOST_TEST
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <sys/loader_firmware_quiet.h>
#include <sys/systm.h>
#include <vm/memory_types.h>
#endif
#include "giulia_rpmh_probe.h"
#include "giulia_arc.h"
extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, unsigned int);
extern void ml_io_unmap(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);
extern void loader_log_line(const char *);
#define GR_COUNT 23u
#define GR_IRQ (1u << 5)
struct gr_mapping { uintptr_t allocation, address; vm_size_t size; };
static struct {
    struct gr_mapping shared, rsc, db, gic;
    volatile struct loader_firmware_quiet *quiet;
    struct giulia_rpmh rpmh;
    uint32_t address[GR_COUNT], raw[2][GR_COUNT], observed[2];
    uint32_t irq_dirty, irq_prior, guard_failed, reads, version;
} gr;
static const char *const gr_names[9]={"xo.lvl","cx.lvl","ldob15","ldoc3",
    "smpi4","smpc2","smpc3","smpc6","smpc1"};
static void gr_log(const char *s) { loader_log_line(s); printf("%s",s); }
static void gr_barrier(__unused void *cookie)
{
#ifndef GIULIA_RPMH_PROBE_HOST_TEST
    __asm__ volatile("dsb sy" ::: "memory");
#else
    __sync_synchronize();
#endif
}
static void gr_map(struct gr_mapping *m,uint64_t pa,vm_size_t bytes,unsigned flags)
{
    vm_offset_t page=trunc_page(pa),off=pa-page;
    m->size=round_page(bytes+off);
    m->allocation=ml_io_map_unmappable(page,m->size,flags);
    if (m->allocation) m->address=m->allocation+off;
}
static void gr_unmap(struct gr_mapping *m)
{
    if (m->allocation) ml_io_unmap(m->allocation,m->size);
    *m=(struct gr_mapping){0};
}
static uint32_t gr_mmio(struct gr_mapping *m,uint32_t off)
{
    uint32_t v=*(volatile uint32_t *)(m->address+off);
    gr_barrier(0); return v;
}
static void gr_store(struct gr_mapping *m,uint32_t off,uint32_t v)
{ *(volatile uint32_t *)(m->address+off)=v; gr_barrier(0); }
static int gr_quiet(void)
{ return gr.quiet->request==1 && gr.quiet->ack==1 && gr.quiet->active==1; }
static int gr_request(unsigned on)
{
    gr.quiet->request=on; gr_barrier(0);
    for (unsigned i=0;i<10000;i++) {
        if (gr.quiet->ack==on && gr.quiet->active==on) return 1;
        delay(100);
    }
    return 0;
}
static int gr_rwp(void)
{
    for (unsigned i=0;i<1000;i++) {
        uint32_t v=gr_mmio(&gr.gic,0);
        if (v==UINT32_MAX) return 0;
        if (!(v&(1u<<31))) return 1;
        delay(10);
    }
    return 0;
}
static int gr_irq_acquire(void)
{
    uint32_t arch=gr_mmio(&gr.gic,0xffe8),enabled=gr_mmio(&gr.gic,0x104);
    uint32_t active=gr_mmio(&gr.gic,0x304),pending=gr_mmio(&gr.gic,0x204);
    if (!gr_quiet() || arch==UINT32_MAX || enabled==UINT32_MAX ||
        active==UINT32_MAX || pending==UINT32_MAX ||
        ((arch&0xf0)!=0x30 && (arch&0xf0)!=0x40) ||
        ((active|pending)&GR_IRQ) || !gr_rwp()) return 0;
    gr.irq_prior=enabled&GR_IRQ; gr.irq_dirty=1;
    gr_store(&gr.gic,0x184,GR_IRQ);
    if (!gr_rwp()) return 0;
    enabled=gr_mmio(&gr.gic,0x104); active=gr_mmio(&gr.gic,0x304);
    return enabled!=UINT32_MAX && active!=UINT32_MAX && !((enabled|active)&GR_IRQ);
}
static int gr_guard(void)
{
    uint32_t enabled=gr_mmio(&gr.gic,0x104),active=gr_mmio(&gr.gic,0x304);
    return !gr.guard_failed && gr_quiet() && gr.irq_dirty && enabled!=UINT32_MAX &&
        active!=UINT32_MAX && !((enabled|active)&GR_IRQ);
}
static uint32_t gr_read(void *cookie,uint32_t off)
{
    (void)cookie;
    if (!gr_guard()) { gr.guard_failed=1; return UINT32_MAX; }
    return gr_mmio(&gr.rsc,off);
}
static void gr_write(void *cookie,uint32_t off,uint32_t v)
{
    (void)cookie;
    if (!gr_guard()) { gr.guard_failed=1; return; }
    gr_store(&gr.rsc,off,v);
}
static void gr_delay(__unused void *cookie,unsigned us) { delay(us); }
static int gr_irq_release(void)
{
    uint32_t enabled,active,pending;
    if (!gr.irq_dirty) return 1;
    if (!gr_quiet() || giulia_rpmh_bus_owned(&gr.rpmh) || !gr_rwp()) return 0;
    enabled=gr_mmio(&gr.gic,0x104); active=gr_mmio(&gr.gic,0x304);
    pending=gr_mmio(&gr.gic,0x204);
    /* Never erase a pending/active interrupt to make cleanup appear clean. */
    if (enabled==UINT32_MAX || active==UINT32_MAX || pending==UINT32_MAX ||
        ((active|pending)&GR_IRQ)) return 0;
    if ((enabled&GR_IRQ)!=gr.irq_prior) {
        gr_store(&gr.gic,gr.irq_prior ? 0x104:0x184,GR_IRQ);
        if (!gr_rwp()) return 0;
        enabled=gr_mmio(&gr.gic,0x104);
        if (enabled==UINT32_MAX || (enabled&GR_IRQ)!=gr.irq_prior) return 0;
    }
    gr.irq_dirty=0; return 1;
}
static int gr_plan(uint32_t address[GR_COUNT])
{
    struct giulia_arc_resource a;
    const uint8_t *db=(const uint8_t *)gr.db.address;
    for (unsigned i=0;i<9;i++) {
        uint32_t base;
        if (i<2) {
            if (giulia_arc_resolve(db,GIULIA_RPMH_DB_MAX,gr.version,gr_names[i],&a)) return 0;
            address[i]=a.address;
        } else {
            if (giulia_rpmh_resolve_vrm(db,GIULIA_RPMH_DB_MAX,gr.version,gr_names[i],&base)) return 0;
            for (unsigned j=0;j<3;j++) address[2+(i-2)*3+j]=base+j*4;
        }
    }
    for (unsigned i=0;i<GR_COUNT;i++) for (unsigned j=0;j<i;j++)
        if (address[i]==address[j]) return 0;
    return 1;
}
static void gr_dump_slots(void)
{
    char line[192];
    for (unsigned s = 0; s < 3; s++) {
        uint32_t bank = s * 672u;
        uint32_t status = gr_mmio(&gr.rsc, 0xd28u + bank);
        uint32_t enable = gr_mmio(&gr.rsc, 0xd2cu + bank);
        uint32_t control = gr_mmio(&gr.rsc, 0xd24u + bank);
        snprintf(line, sizeof(line), "ace5-rpmh-read: pre-slot=%u status=%#x enable=%#x control=%#x\n",
            s, status, enable, control);
        gr_log(line);
        if (enable == UINT32_MAX || !enable) continue;
        for (unsigned c = 0; c < 16; c++) {
            if (!(enable & (1u << c))) continue;
            uint32_t off = 0xd34u + bank + c * 24u;
            uint32_t msgid = gr_mmio(&gr.rsc, off);
            uint32_t addr = gr_mmio(&gr.rsc, off + 4u);
            uint32_t data = gr_mmio(&gr.rsc, off + 8u);
            uint32_t sts = gr_mmio(&gr.rsc, off + 12u);
            uint32_t resp = gr_mmio(&gr.rsc, off + 16u);
            snprintf(line, sizeof(line),
                "ace5-rpmh-read: pre-cmd slot=%u cmd=%u msgid=%#x addr=%#x data=%#x sts=%#x resp=%#x\n",
                s, c, msgid, addr, data, sts, resp);
            gr_log(line);
        }
    }
}
static void gr_thread(void *arg,__unused wait_result_t wait)
{
    uint64_t pa=(uint64_t)(uintptr_t)arg;
    struct giulia_rpmh_io io={0,gr_read,gr_write,gr_barrier,gr_delay};
    uint32_t again[GR_COUNT]; int valid=0,rc=0,quiet=0,clean=0;
    char line[192];
    gr_map(&gr.shared,pa,sizeof(struct loader_firmware_quiet),VM_WIMG_WCOMB);
    if (!gr.shared.address) goto done;
    gr.quiet=(volatile struct loader_firmware_quiet *)gr.shared.address;
    if (gr.quiet->magic!=LOADER_QUIET_MAGIC || gr.quiet->version!=LOADER_QUIET_VERSION ||
        gr.quiet->size!=sizeof(*gr.quiet) || gr.quiet->request || gr.quiet->ack || gr.quiet->active) goto done;
    gr_map(&gr.rsc,0x17a20000u,0x10000u,VM_WIMG_IO);
    gr_map(&gr.db,0x81c60000u,GIULIA_RPMH_DB_MAX,VM_WIMG_WCOMB);
    gr_map(&gr.gic,0x17100000u,0x10000u,VM_WIMG_IO);
    if (!gr.rsc.address || !gr.db.address || !gr.gic.address) goto done;
    gr.version=*(volatile uint32_t *)gr.db.address;
    if (!gr_plan(gr.address)) { gr_log("ace5-rpmh-read: current CMD DB rejected; no request sent\n"); goto done; }
    quiet=1;
    if (!gr_request(1)) goto release;
    if (!gr_irq_acquire()) { gr_log("ace5-rpmh-read: IRQ37 lease rejected; no request sent\n"); goto release; }
    gr_dump_slots();
    /* Actual firmware quiet ACK, single XNU CPU/incompatible-driver admission,
     * common RSC token and real IRQ37 mask establish these local conditions.
     * Other firmware masters remain live; no remote resource owner is granted. */
    if ((rc=giulia_rpmh_init(&gr.rpmh,&io,GIULIA_RPMH_REQUIRED))) goto release;
    for (unsigned pass=0;pass<2;pass++) for (unsigned n=0;n<GR_COUNT;n++) {
        unsigned i=pass ? GR_COUNT-1-n:n;
        if (!gr_guard()) { rc=GIULIA_RPMH_PREREQUISITE; goto report; }
        rc=giulia_rpmh_read(&gr.rpmh,gr.address[i]);
        if (rc || (rc=giulia_rpmh_read_result(&gr.rpmh,&gr.raw[pass][i]))) goto report;
        gr.observed[pass]|=1u<<i;
        gr.reads++;
    }
    valid=gr_plan(again) && !memcmp(again,gr.address,sizeof(again)) &&
        !memcmp(gr.raw[0],gr.raw[1],sizeof(gr.raw[0]));
report:
    snprintf(line,sizeof(line),"ace5-rpmh-read: rc=%d stable=%u reads=%u pending=%u guard=%u irq=%#x tcs=%#x cmd=%#x\n",
        rc,valid,gr.reads,gr.rpmh.triggered&&!gr.rpmh.completed,gr.guard_failed,
        gr.rpmh.last_irq,gr.rpmh.last_tcs,gr.rpmh.last_command); gr_log(line);
    snprintf(line,sizeof(line),"ace5-rpmh-read: busy_reason=%u irq=%#x slot=%u status=%#x enable=%#x control=%#x triggered=%u selected=%u\n",
        gr.rpmh.busy_reason,gr.rpmh.busy_irq,gr.rpmh.busy_slot,gr.rpmh.busy_status,
        gr.rpmh.busy_enable,gr.rpmh.busy_control,gr.rpmh.triggered,gr.rpmh.slot); gr_log(line);
    for (unsigned i=0;i<GR_COUNT;i++) {
        snprintf(line,sizeof(line),"ace5-rpmh-read: item=%u addr=%#x first=%#x second=%#x observed=%u/%u\n",
            i,gr.address[i],gr.raw[0][i],gr.raw[1][i],(gr.observed[0]>>i)&1u,(gr.observed[1]>>i)&1u); gr_log(line);
    }
release:
    if (giulia_rpmh_bus_owned(&gr.rpmh) || !gr_irq_release()) goto retain;
    if (quiet && !gr_request(0)) goto retain;
    clean=quiet;
    if (valid) gr_log("ace5-rpmh-read: capture complete; 46 READ replies stable; resource ownership NOT assumed\n");
done:
    gr_unmap(&gr.gic); gr_unmap(&gr.db); gr_unmap(&gr.rsc); gr_unmap(&gr.shared);
    if (clean) gr_log("ace5-rpmh-read: cleanup complete; IRQ restored and firmware resumed\n");
    thread_terminate(current_thread()); return;
retain:
    gr_log("ace5-rpmh-read: incomplete; IRQ/mappings/firmware lease retained; cold reboot required\n");
    thread_terminate(current_thread());
}
void giulia_rpmh_read_test_start(uint64_t pa)
{
    thread_t thread;
    if (kernel_thread_start(gr_thread,(void *)(uintptr_t)pa,&thread)==KERN_SUCCESS)
        thread_deallocate(thread);
    else gr_log("ace5-rpmh-read: no test thread\n");
}
