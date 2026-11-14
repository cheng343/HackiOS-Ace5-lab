/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_PCIE_MSI_XNU_H
#define GIULIA_PCIE_MSI_XNU_H
#include "giulia_pcie_msi.h"

/* Board-service adapter, not an automatic Wi-Fi init hook. The board must
 * provide live Device-memory mappings and exclusive ownership of the entire
 * RC MSI receiver and GIC INTID 173. Never pass mappings from a powered-off
 * PCIe domain. Start and stop run on the same CPU, outside its IRQ handler.
 * All other operations are serialized with interrupts off on that CPU.
 * Zero-initialize once; the object and mappings must outlive the IRQ handler.
 * The board serializes start/object lifetime (including failed start).
 */
struct giulia_pcie_msi_xnu {
    struct giulia_pcie_msi core;
    uintptr_t dbi, cfg;
    int owner_cpu;
    unsigned owned, attached, in_handler;
    uint64_t interrupts, delivered;
    void (*deliver)(void *, unsigned);
    void *cookie;
};

int giulia_pcie_msi_xnu_start(struct giulia_pcie_msi_xnu *,
    const struct giulia_pcie_msi_config *, uintptr_t dbi, uint32_t dbi_size,
    uintptr_t cfg, uint32_t cfg_size, uint32_t enable_mask,
    void (*deliver)(void *, unsigned), void *cookie);
int giulia_pcie_msi_xnu_mask(struct giulia_pcie_msi_xnu *, uint32_t bits);
int giulia_pcie_msi_xnu_unmask(struct giulia_pcie_msi_xnu *, uint32_t bits);
/* Failure retains owned==1 and mappings; retry on owner CPU while PCIe lives.
 * This is an orderly-stop API, not a panic/remote-CPU teardown implementation.
 * Success leaves the receiver disabled; it does not restore another OS's
 * active MSI configuration. Only a previously quiescent receiver is admitted.
 */
int giulia_pcie_msi_xnu_stop(struct giulia_pcie_msi_xnu *);
#endif
