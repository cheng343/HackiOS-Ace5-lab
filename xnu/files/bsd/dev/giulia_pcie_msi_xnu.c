/* SPDX-License-Identifier: BSD-2-Clause */
/* Ace 5 PCIe0 MSI: real MMIO and existing XNU GIC SPI service adapter.
 * There is deliberately no startup registration: PCIe power/link/ownership
 * and a tested local MSI route are prerequisites supplied by the board.
 */
#ifndef GIULIA_PCIE_MSI_XNU_HOST_TEST
#include <mach/mach_types.h>
#include <kern/cpu_data.h> /* XNU 8796 preemption declarations */
#include <machine/machine_routines.h>
#include <machine/cpu_number.h> /* exported to the BSD build, unlike cpu_data_internal */
#endif
#include <stdbool.h>
#include "giulia_pcie_msi_xnu.h"

extern kern_return_t gic_v3_spi_attach(uint32_t, bool,
    void (*)(void *, uint32_t), void *);
extern void gic_v3_spi_mask(uint32_t);
extern void gic_v3_spi_unmask(uint32_t);
extern void gic_v3_spi_detach(uint32_t);

static uint32_t gx_read_dbi(void *cookie, uint32_t offset)
{
    struct giulia_pcie_msi_xnu *x = cookie;
    return *(volatile uint32_t *)(x->dbi + offset);
}
static void gx_write_dbi(void *cookie, uint32_t offset, uint32_t value)
{
    struct giulia_pcie_msi_xnu *x = cookie;
    *(volatile uint32_t *)(x->dbi + offset) = value;
}
static uint16_t gx_read_cfg16(void *cookie, uint32_t offset)
{
    struct giulia_pcie_msi_xnu *x = cookie;
    return *(volatile uint16_t *)(x->cfg + offset);
}
static void gx_write_cfg16(void *cookie, uint32_t offset, uint16_t value)
{
    struct giulia_pcie_msi_xnu *x = cookie;
    *(volatile uint16_t *)(x->cfg + offset) = value;
}
static uint32_t gx_read_cfg32(void *cookie, uint32_t offset)
{
    struct giulia_pcie_msi_xnu *x = cookie;
    return *(volatile uint32_t *)(x->cfg + offset);
}
static void gx_write_cfg32(void *cookie, uint32_t offset, uint32_t value)
{
    struct giulia_pcie_msi_xnu *x = cookie;
    *(volatile uint32_t *)(x->cfg + offset) = value;
}
static void gx_barrier(void *cookie)
{
    (void)cookie;
#ifndef GIULIA_PCIE_MSI_XNU_HOST_TEST
    __asm__ volatile("dsb sy" ::: "memory");
#else
    __sync_synchronize();
#endif
}

static void gx_deliver(void *cookie, unsigned vector)
{
    struct giulia_pcie_msi_xnu *x = cookie;
    x->delivered++;
    x->deliver(x->cookie, vector);
}
static void gx_irq(void *cookie, uint32_t intid)
{
    struct giulia_pcie_msi_xnu *x = cookie;
    if (intid != GIULIA_PCIE_MSI_INTID) return;
    if (!x->owned || !x->attached || cpu_number() != x->owner_cpu) {
        gic_v3_spi_mask(intid);
        return;
    }
    x->interrupts++;
    x->in_handler = 1;
    (void)giulia_pcie_msi_dispatch(&x->core, gx_deliver, x);
    x->in_handler = 0;
    /* A failed source-stop leaves its receiver retained and masked. */
    if (x->core.state == GIULIA_MSI_FAULT) gic_v3_spi_mask(intid);
}

/* Called with preemption and local interrupts disabled, outside our handler. */
static int gx_stop(struct giulia_pcie_msi_xnu *x)
{
    int result = giulia_pcie_msi_stop(&x->core);
    if (x->attached) gic_v3_spi_mask(GIULIA_PCIE_MSI_INTID);
    if (result != GIULIA_MSI_OK) return result;
    if (x->attached) {
        /* No handler can be running: IRQ is routed to this CPU and this call
         * cannot run in gx_irq. Avoid detach on a foreign/in-flight CPU. */
        gic_v3_spi_detach(GIULIA_PCIE_MSI_INTID);
        x->attached = 0;
    }
    x->owned = 0;
    return GIULIA_MSI_OK;
}

int giulia_pcie_msi_xnu_start(struct giulia_pcie_msi_xnu *x,
    const struct giulia_pcie_msi_config *config,
    uintptr_t dbi, uint32_t dbi_size, uintptr_t cfg, uint32_t cfg_size,
    uint32_t enable_mask, void (*deliver)(void *, unsigned), void *cookie)
{
    struct giulia_pcie_msi_io io;
    boolean_t interrupts;
    int result;
    if (!x || !config || !deliver || !dbi || !cfg || (dbi & 3u) ||
        (cfg & 3u) || dbi_size < 0x900u || cfg_size < 0x100u ||
        dbi > UINTPTR_MAX - dbi_size || cfg > UINTPTR_MAX - cfg_size)
        return GIULIA_MSI_ARGUMENT;
    if (x->owned || x->attached || x->in_handler) return GIULIA_MSI_STATE;
    disable_preemption();
    interrupts = ml_set_interrupts_enabled(FALSE);
    x->dbi = dbi;
    x->cfg = cfg;
    x->owner_cpu = cpu_number();
    x->deliver = deliver;
    x->cookie = cookie;
    x->interrupts = x->delivered = 0;
    x->owned = 1;
    io = (struct giulia_pcie_msi_io) {
        x, gx_read_dbi, gx_write_dbi, gx_read_cfg16, gx_write_cfg16,
        gx_read_cfg32, gx_write_cfg32, gx_barrier
    };
    result = giulia_pcie_msi_prepare(&x->core, config, &io);
    if (result != GIULIA_MSI_OK) {
        if (x->core.state == GIULIA_MSI_IDLE ||
            x->core.state == GIULIA_MSI_STOPPED) x->owned = 0;
        goto out;
    }
    if (gic_v3_spi_attach(GIULIA_PCIE_MSI_INTID, false, gx_irq, x) != KERN_SUCCESS) {
        result = GIULIA_MSI_PREREQUISITE;
        /* No IRQ was acquired: never detach somebody else's slot. */
        (void)gx_stop(x);
        goto out;
    }
    x->attached = 1;
    /* attach enables the SPI, but local IRQs and the prepared RC are masked. */
    gic_v3_spi_mask(GIULIA_PCIE_MSI_INTID);
    result = giulia_pcie_msi_arm(&x->core, GIULIA_PCIE_MSI_INTID, enable_mask);
    if (result != GIULIA_MSI_OK) {
        (void)gx_stop(x);
        goto out;
    }
    gic_v3_spi_unmask(GIULIA_PCIE_MSI_INTID);
out:
    ml_set_interrupts_enabled(interrupts);
    enable_preemption();
    return result;
}

static int gx_change_mask(struct giulia_pcie_msi_xnu *x, uint32_t bits, bool mask)
{
    boolean_t interrupts;
    int result;
    if (!x) return GIULIA_MSI_ARGUMENT;
    disable_preemption();
    interrupts = ml_set_interrupts_enabled(FALSE);
    if (!x->owned || !x->attached || cpu_number() != x->owner_cpu)
        result = GIULIA_MSI_STATE;
    else
        result = mask ? giulia_pcie_msi_mask(&x->core, bits) :
            giulia_pcie_msi_unmask(&x->core, bits);
    ml_set_interrupts_enabled(interrupts);
    enable_preemption();
    return result;
}
int giulia_pcie_msi_xnu_mask(struct giulia_pcie_msi_xnu *x, uint32_t bits)
{
    return gx_change_mask(x, bits, true);
}
int giulia_pcie_msi_xnu_unmask(struct giulia_pcie_msi_xnu *x, uint32_t bits)
{
    return gx_change_mask(x, bits, false);
}
int giulia_pcie_msi_xnu_stop(struct giulia_pcie_msi_xnu *x)
{
    boolean_t interrupts;
    int result;
    if (!x) return GIULIA_MSI_ARGUMENT;
    disable_preemption();
    interrupts = ml_set_interrupts_enabled(FALSE);
    if (!x->owned) result = GIULIA_MSI_OK;
    else if (x->in_handler || cpu_number() != x->owner_cpu)
        result = GIULIA_MSI_STATE;
    else result = gx_stop(x);
    ml_set_interrupts_enabled(interrupts);
    enable_preemption();
    return result;
}
