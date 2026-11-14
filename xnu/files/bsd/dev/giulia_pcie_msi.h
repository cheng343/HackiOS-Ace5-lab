/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_PCIE_MSI_H
#define GIULIA_PCIE_MSI_H

#include <stdint.h>

/* SM8650 PCIe0 SNPS group 0: DT SPI 141, architectural GIC INTID 173.
 * This interface never maps memory, allocates a target, or attaches an IRQ.
 * All operations, including dispatch, must be serialized by the caller.
 * The caller retains powered/clocked DBI and endpoint config mappings until
 * stop succeeds. A failed stop must be retried, not followed by unmapping.
 */
#define GIULIA_PCIE_MSI_INTID 173u
#define GIULIA_PCIE_MSI_VECTORS 32u

enum giulia_pcie_msi_result {
    GIULIA_MSI_OK = 0,
    GIULIA_MSI_ARGUMENT = -1,
    GIULIA_MSI_PREREQUISITE = -2,
    GIULIA_MSI_STATE = -3,
    GIULIA_MSI_CAPABILITY = -4,
    GIULIA_MSI_SOURCE_ENABLED = -5,
    GIULIA_MSI_READBACK = -6,
    GIULIA_MSI_RECEIVER_BUSY = -7
};
enum giulia_pcie_msi_state {
    GIULIA_MSI_IDLE = 0, GIULIA_MSI_PREPARED, GIULIA_MSI_ACTIVE,
    GIULIA_MSI_STOPPED, GIULIA_MSI_FAULT
};
enum giulia_pcie_msi_prerequisite {
    GIULIA_MSI_PCIE_READY = 1u << 0,
    GIULIA_MSI_DBI_EXCLUSIVE = 1u << 1,
    GIULIA_MSI_SNPS_ROUTE_CONFIRMED = 1u << 2,
    GIULIA_MSI_TARGET_RESERVED = 1u << 3
};
#define GIULIA_MSI_REQUIRED (GIULIA_MSI_PCIE_READY | \
    GIULIA_MSI_DBI_EXCLUSIVE | GIULIA_MSI_SNPS_ROUTE_CONFIRMED | \
    GIULIA_MSI_TARGET_RESERVED)

struct giulia_pcie_msi_io {
    void *cookie;
    uint32_t (*read_dbi32)(void *, uint32_t);
    void (*write_dbi32)(void *, uint32_t, uint32_t);
    uint16_t (*read_cfg16)(void *, uint32_t);
    void (*write_cfg16)(void *, uint32_t, uint16_t);
    uint32_t (*read_cfg32)(void *, uint32_t);
    void (*write_cfg32)(void *, uint32_t, uint32_t);
    void (*barrier)(void *);
};
struct giulia_pcie_msi_config {
    /* PCIe bus address, never an Android IOVA borrowed from a previous boot.
     * Caller reserves a collision-free four-byte target below 4 GiB, outside
     * endpoint BARs and external MSI/ITS targets until stop succeeds. A
     * reserved CFG0 bus address can be used; a RAM/DMA allocation is not
     * required. SNPS intercepts this
     * address in its own receiver; TARGET_RESERVED is an ownership assertion,
     * not permission to truncate a 64-bit allocation or reuse the ITS target.
     */
    uint64_t target;
    uint32_t prerequisites;
    uint16_t capability; /* Caller-discovered conventional PCI MSI capability. */
    uint8_t vectors;     /* Power of two, 1..32; MSI-X is not supported. */
};
struct giulia_pcie_msi {
    struct giulia_pcie_msi_io io;
    uint32_t state, valid_mask, enabled_mask;
    uint16_t capability, control, data_offset, mask_offset;
    uint8_t has_mask;
};

/* Zero-initialize the object once. No MMIO occurs if prerequisites are absent.
 * prepare requires endpoint MSI and all eight RC groups already disabled;
 * it refuses an occupied receiver even with DBI_EXCLUSIVE asserted. It leaves
 * both ends masked, enabling only receiver group 0's allocated vectors.
 * arm requires the caller to have attached a level IRQ at INTID 173, still
 * masked at the GIC. Enable the GIC only after arm succeeds. On arm/prepare
 * readback failure, source cleanup is attempted; inspect state and retry stop
 * if FAULT. Every int-returning operation returns 0 on success.
 */
int giulia_pcie_msi_prepare(struct giulia_pcie_msi *,
    const struct giulia_pcie_msi_config *, const struct giulia_pcie_msi_io *);
int giulia_pcie_msi_arm(struct giulia_pcie_msi *, uint32_t attached_intid,
    uint32_t enable_mask);
int giulia_pcie_msi_mask(struct giulia_pcie_msi *, uint32_t bits);
int giulia_pcie_msi_unmask(struct giulia_pcie_msi *, uint32_t bits);
/* W1C acknowledge before delivering. One bounded snapshot per GIC entry;
 * a new event during delivery remains pending for the next level interrupt.
 * deliver must not stop/reinitialize this object or release its mappings.
 */
uint32_t giulia_pcie_msi_dispatch(struct giulia_pcie_msi *,
    void (*deliver)(void *, unsigned), void *);
/* Disable endpoint MSI + readback first; then mask/disable/clear the receiver.
 * Only after success may the caller mask/detach GIC and release resources.
 * On failed source disable, retain receiver enable, mask its outputs, mark
 * FAULT and return READBACK. stop is retryable from FAULT and idempotent after
 * STOPPED. Software cannot recover a faulting/unclocked MMIO transaction.
 */
int giulia_pcie_msi_stop(struct giulia_pcie_msi *);

#endif
