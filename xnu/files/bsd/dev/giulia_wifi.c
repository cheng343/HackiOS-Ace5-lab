/* SPDX-License-Identifier: BSD-2-Clause */
/* Ace 5 XNU handoff diagnostics. ace5_wifi_probe=1 reads the known GCC,
 * TLMM and CMD DB/RSC resources; >=2 adds narrow PCIe CESTA/CRM headers;
 * >=3 reads AOSS/QMP descriptor metadata without message payloads;
 * =4 deliberately attempts the vendor diagnostic SM/CRM runtime whitelist.
 * This file has no device-register write operation and enables no radio.
 * Linux/vendor early-probe access is evidence for the intended MMIO path,
 * not a guarantee against hardware aborts in an untested boot environment.
 */
#ifndef GIULIA_WIFI_HOST_TEST
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/hackios_platform.h>
#include <sys/systm.h>
#include <vm/memory_types.h>
#endif
#include "giulia_pcie_noc.h"
#include "giulia_rpmh.h"
#include "giulia_wifi_gpio.h"
#include "giulia_aop.h"
#include "giulia_bcm.h"
#include "giulia_arc.h"

extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, unsigned int);
extern void ml_io_unmap(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);
extern void loader_log_line(const char *);

static unsigned wifi_probe_mode, wifi_probe_started;
struct wifi_mapping { vm_offset_t allocation, address; vm_size_t size; };
struct wifi_probe_map { struct wifi_mapping gcc, gpio, rsc, db, sm, crm, aop; };
static void wifi_log(const char *format, ...) __printflike(1, 2);
static void wifi_log(const char *format, ...)
{
    char line[256];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    loader_log_line(line);
    printf("%s", line);
}
static uint32_t wifi_read(uintptr_t base, uint32_t offset)
{
    uint32_t value = *(volatile uint32_t *)(base + offset);
#ifndef GIULIA_WIFI_HOST_TEST
    __asm__ volatile("dsb sy" ::: "memory");
#else
    __sync_synchronize();
#endif
    return value;
}
/* XNU c54f35ca767986246321eb901baf8f5ff7923f6a:
 * osfmk/arm/io_map.c io_map(false) allocates permanent io_submap entries;
 * osfmk/arm64/machine_routines.c ml_io_unmap frees kernel_map entries.
 * Use the removable API and retain the whole page allocation separately
 * from each resource's address (SM/CRM are not XNU 16K-page aligned).
 */
static void wifi_map(struct wifi_mapping *m, vm_offset_t physical,
    vm_size_t size, unsigned int flags)
{
    vm_offset_t page = trunc_page(physical), offset = physical - page;
    vm_size_t span = round_page(size + offset);
    vm_offset_t allocation = ml_io_map_unmappable(page, span, flags);
    if (!allocation) return;
    m->allocation = allocation; m->address = allocation + offset; m->size = span;
}
static uint32_t wifi_aop_read(void *cookie, uint32_t offset)
{ return wifi_read((uintptr_t)cookie, offset); }
static int wifi_noc_read(void *cookie, unsigned block, uint32_t offset, uint32_t *value)
{
    struct wifi_probe_map *m = cookie;
    *value = wifi_read(block == GIULIA_NOC_SM ? m->sm.address : m->crm.address, offset);
    return *value == UINT32_MAX ? -1 : 0;
}
static void wifi_unmap(struct wifi_mapping *m)
{
    if (m->allocation) ml_io_unmap(m->allocation, m->size);
    *m = (struct wifi_mapping){0};
}
static void wifi_probe_thread(__unused void *arg, __unused wait_result_t wait)
{
    struct wifi_probe_map m = {0};
    static const uint32_t gcc_offsets[] = {
        0x6b004, 0x6b008, 0x6c000, 0x6c004, 0x5214c, 0x52000,
        0x52008, 0x6b074, 0x6b078, 0x6b058, 0x6b05c, 0x6b070,
        0x10058, 0x10064, 0x10074, 0x6b01c, 0x6b020, 0x6b02c,
        0x6b038, 0x6b03c, 0x6b054, 0x63004, 0x63008
    };
    static const unsigned gpio_pins[] = {16, 17, 18, 19, 94, 95, 96};
    static const char *const rail_names[] = {
        "ldob15", "ldoc3", "smpi4", "smpc2", "smpc3", "smpc6", "smpc1"
    };
    wifi_log("ace5-wifi: probe=%u read-only begin; no rail/GPIO/PCIe writes\n", wifi_probe_mode);
    wifi_map(&m.gcc, 0x00100000u, 0x70000u, VM_WIMG_IO);
    wifi_map(&m.gpio, GIULIA_WIFI_GPIO_PA, GIULIA_WIFI_GPIO_SIZE, VM_WIMG_IO);
    wifi_map(&m.rsc, 0x17a20000u, 0x10000u, VM_WIMG_IO);
    wifi_map(&m.db, 0x81c60000u, GIULIA_RPMH_DB_MAX, VM_WIMG_WCOMB);
    if (!m.gcc.address || !m.gpio.address || !m.rsc.address || !m.db.address) {
        wifi_log("ace5-wifi: baseline mapping failed\n"); goto out;
    }
    wifi_log("ace5-wifi: baseline GCC/TLMM/RSC reads start\n");
    for (unsigned i = 0; i < sizeof(gcc_offsets) / sizeof(gcc_offsets[0]); ++i)
        wifi_log("ace5-wifi: GCC +%#x=%#x\n", gcc_offsets[i], wifi_read(m.gcc.address, gcc_offsets[i]));
    for (unsigned i = 0; i < sizeof(gpio_pins) / sizeof(gpio_pins[0]); ++i) {
        unsigned pin = gpio_pins[i];
        wifi_log("ace5-wifi: GPIO%u cfg=%#x io=%#x irq=%#x\n", pin,
            wifi_read(m.gpio.address, pin * 0x1000u), wifi_read(m.gpio.address, pin * 0x1000u + 4u),
            wifi_read(m.gpio.address, pin * 0x1000u + 8u));
    }
    uint32_t rsc_id = wifi_read(m.rsc.address, 0);
    uint32_t db_version = wifi_read(m.db.address, 0), db_magic = wifi_read(m.db.address, 4);
    wifi_log("ace5-wifi: RSC id=%#x CMD-DB version=%#x magic=%#x\n", rsc_id, db_version, db_magic);
    /* Record this boot's observed header identity, not a guessed version 1.
     * Resolution is structural inspection only: no prior vote/rail state is
     * inferred from a CMD DB entry, and nothing calls rails_start here. */
    if (db_version && db_version != UINT32_MAX && db_magic == 0x0c0330dbu) {
        for (unsigned i = 0; i < sizeof(rail_names) / sizeof(rail_names[0]); ++i) {
            uint32_t address = 0;
            int error = giulia_rpmh_resolve_vrm((const uint8_t *)m.db.address,
                GIULIA_RPMH_DB_MAX, db_version, rail_names[i], &address);
            wifi_log("ace5-wifi: CMD-DB %s rc=%d resource=%#x (prior vote unknown)\n",
                rail_names[i], error, address);
        }
        const char *const arc_names[]={"xo.lvl","cx.lvl"};
        for (unsigned i=0;i<2;i++) {
            struct giulia_arc_resource arc={0};
            uint32_t selector=UINT32_MAX;
            int error=giulia_arc_resolve((const uint8_t *)m.db.address,
                GIULIA_RPMH_DB_MAX,db_version,arc_names[i],&arc);
            if (!error) {
                if (i) error=giulia_arc_select(&arc,64,&selector);
                else if (arc.count>3) selector=3; /* Vendor XO ON protocol. */
                else error=GIULIA_RPMH_DATABASE;
            }
            wifi_log("ace5-wifi: ARC %s rc=%d resource=%#x levels=%u selector=%u corner=%u (prior vote unknown)\n",
                arc_names[i],error,arc.address,arc.count,selector,
                !error ? arc.level[selector]:0);
        }
    }
    if (wifi_probe_mode >= 2) {
        /* Vendor crm_probe reads ENABLE/VERSION/CFG immediately after map,
         * with no clock/regulator/ICC operation. pci-msm pcie_init accesses
         * SM overrides before driver registration/enumeration. These exact
         * early resources match the observed giulia DT; no inaccessible
         * PCIe DBI/BAR or PHY aperture is touched by this diagnostic.
         * References are pinned in giulia_pcie_noc.c. */
        wifi_map(&m.sm, GIULIA_NOC_SM_PHYS, GIULIA_NOC_SM_SIZE, VM_WIMG_IO);
        wifi_map(&m.crm, GIULIA_NOC_CRM_PHYS, GIULIA_NOC_CRM_SIZE, VM_WIMG_IO);
        if (!m.sm.address || !m.crm.address) { wifi_log("ace5-wifi: CESTA/CRM mapping failed\n"); goto out; }
        /* Only the registers accessed during vendor early init are sampled.
         * Runtime SM PC/status, channel tables and PERF/BW status require
         * separately established access; a readable header does not prove it. */
        wifi_log("ace5-wifi: narrow early CESTA/CRM read starts\n");
        const uint32_t sm_offsets[] = {0x1040u, 0x1048u};
        for (unsigned i = 0; i < 2; ++i) {
            uint32_t value = wifi_read(m.sm.address, sm_offsets[i]);
            wifi_log("ace5-wifi: early SM +%#x=%#x\n", sm_offsets[i], value);
            if (value == UINT32_MAX) goto out;
        }
        for (unsigned i = 0; i < 4; ++i) {
            uint32_t value = wifi_read(m.crm.address, i * 4u);
            wifi_log("ace5-wifi: early CRM +%#x=%#x\n", i * 4u, value);
            if (value == UINT32_MAX) goto out;
        }
        wifi_log("ace5-wifi: early CESTA/CRM headers captured; full runtime snapshot deferred\n");
    }
    if (wifi_probe_mode >= 3) {
        struct giulia_aop_snapshot s;
        struct giulia_aop_io io = {0};
        int rc;
        wifi_map(&m.aop, GIULIA_AOP_MSGRAM_PHYS, GIULIA_AOP_MSGRAM_SIZE, VM_WIMG_IO);
        if (!m.aop.address) { wifi_log("ace5-wifi: AOP mapping failed\n"); goto out; }
        io.cookie = (void *)m.aop.address; io.read32 = wifi_aop_read;
        rc = giulia_aop_inspect(&io, GIULIA_AOP_MSGRAM_SIZE, &s);
        wifi_log("ace5-wifi: AOP descriptor rc=%d changed=%u\n", rc, s.changed);
        for (unsigned i = 0; i < 15; i++)
            wifi_log("ace5-wifi: AOP +%#x=%#x\n", i * 4u, s.words[i]);
        if (rc || s.changed) goto out;
        wifi_log("ace5-wifi: AOP mailbox length=%u; payload not read\n", s.message_length);
        wifi_log("ace5-wifi: AOP descriptor captured; channel ownership NOT assumed\n");
    }
    if (wifi_probe_mode == 4) {
        struct giulia_pcie_noc_snapshot s;
        const struct giulia_pcie_noc_io io = {&m,wifi_noc_read,
            GIULIA_NOC_SM_PHYS,GIULIA_NOC_SM_SIZE,GIULIA_NOC_CRM_PHYS,GIULIA_NOC_CRM_SIZE};
        /* Explicit diagnostic attempt of the vendor SM/CRM dump registers.
         * Their fixed live-DT resource and early headers were checked above;
         * crm.c maps/uses this block without enabling a separate power path.
         * An actual bus abort cannot be caught by this polling reader. No
         * DBI/PHY/BAR, CLKREQ command, vote or channel write is attempted. */
        wifi_log("ace5-wifi: full SM/CRM read-only capture starts\n");
        int rc = giulia_pcie_noc_snapshot(&io,GIULIA_NOC_REQUIRED,&s);
        wifi_log("ace5-wifi: full SM/CRM rc=%d valid=%#x reads=%u fail=%u/%#x\n",
            rc,s.valid,s.read_count,s.failed_block,s.failed_offset);
        if (rc) goto out;
        wifi_log("ace5-wifi: CRM channels=%u states=%u perf=%u bw=%u busy=%#x update=%#x after=%#x/%#x\n",
            s.channels,s.states,s.perf_resources,s.bw_resources,s.channel[0],s.channel[1],
            s.channel_after[0],s.channel_after[1]);
        for (unsigned i=0;i<GIULIA_NOC_SM_REGS;i++)
            wifi_log("ace5-wifi: SM index=%u before=%#x after=%#x\n",i,s.sm[i],s.sm_after[i]);
        for (unsigned ch=0;ch<s.channels;ch++) for (unsigned st=0;st<s.states;st++) {
            for (unsigned i=0;i<s.perf_resources;i++)
                wifi_log("ace5-wifi: CRM perf ch=%u st=%u res=%u raw=%#x\n",ch,st,i,s.perf[ch][st][i]);
            for (unsigned i=0;i<s.bw_resources;i++)
                wifi_log("ace5-wifi: CRM bw ch=%u st=%u res=%u raw=%#x\n",ch,st,i,s.bw[ch][st][i]);
        }
        for (unsigned i=0;i<s.perf_resources;i++)
            wifi_log("ace5-wifi: CRM perf-status res=%u raw=%#x\n",i,s.perf_status[i]);
        wifi_log("ace5-wifi: CRM power-index=%#x\n",s.pwr_idx_status);
        if (db_version && db_version != UINT32_MAX && db_magic == 0x0c0330dbu) {
            const char *const names[] = {"MC0","SH0","SH1","SN4","ACV","CN0"};
            for (unsigned i=0;i<6;i++) {
                struct giulia_bcm_resource resource={0};
                int error=giulia_bcm_resolve((const uint8_t *)m.db.address,
                    GIULIA_RPMH_DB_MAX,db_version,names[i],&resource);
                wifi_log("ace5-wifi: BCM %s rc=%d address=%#x unit=%u width=%u vcd=%u\n",
                    names[i],error,resource.address,resource.unit,resource.width,resource.vcd);
            }
            wifi_log("ace5-wifi: BCM metadata inspection complete; no votes sent\n");
        }
        wifi_log("ace5-wifi: full SM/CRM snapshot captured; no table/channel writes\n");
    }
    wifi_log("ace5-wifi: read-only probe complete; NoC/CESTA ownership and rail baselines NOT assumed\n");
out:
    wifi_unmap(&m.aop);
    wifi_unmap(&m.crm); wifi_unmap(&m.sm);
    wifi_unmap(&m.db); wifi_unmap(&m.rsc);
    wifi_unmap(&m.gpio); wifi_unmap(&m.gcc);
    thread_terminate(current_thread());
}
void giulia_wifi_init(void)
{
    unsigned mode = 0;
    thread_t thread;
    if (!PE_parse_boot_argn("ace5_wifi_probe", &mode, sizeof(mode)) || !mode) return;
    if (hackios_platform() != HACKIOS_PLATFORM_GIULIA || mode > 4) {
        wifi_log("ace5-wifi: unsupported board/probe mode; no MMIO\n"); return;
    }
    if (__atomic_exchange_n(&wifi_probe_started, 1, __ATOMIC_ACQ_REL)) return;
    wifi_probe_mode = mode;
    if (kernel_thread_start(wifi_probe_thread, NULL, &thread) == KERN_SUCCESS)
        thread_deallocate(thread);
    else wifi_log("ace5-wifi: no diagnostic thread\n");
}
