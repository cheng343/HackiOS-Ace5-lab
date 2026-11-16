/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_PCIE_QOS_H
#define GIULIA_PCIE_QOS_H
#include <stdint.h>

/* Fixed physical PCIe0 -> PCIe ANOC -> GEM NoC path. The virtual CRM nodes
 * refer to this same hardware, not separate QoS registers. */
#define GIULIA_QOS_GCC_PHYS 0x00100000u
#define GIULIA_QOS_GCC_SIZE 0x001f4200u
#define GIULIA_QOS_PCIE_PHYS 0x016c0000u
#define GIULIA_QOS_PCIE_SIZE 0x00012200u
#define GIULIA_QOS_GEM_PHYS 0x24100000u
#define GIULIA_QOS_GEM_SIZE 0x000c5080u
enum giulia_pcie_qos_block { GIULIA_QOS_GCC, GIULIA_QOS_PCIE, GIULIA_QOS_GEM };
enum giulia_pcie_qos_state {
    GIULIA_QOS_IDLE, GIULIA_QOS_PREPARING, GIULIA_QOS_ACTIVE,
    GIULIA_QOS_FAULT, GIULIA_QOS_STOPPED
};
enum giulia_pcie_qos_result {
    GIULIA_QOS_ARGUMENT = -1, GIULIA_QOS_STATE = -2,
    GIULIA_QOS_LAYOUT = -3, GIULIA_QOS_PREREQUISITE = -4,
    GIULIA_QOS_LEASE = -5, GIULIA_QOS_IO = -6, GIULIA_QOS_READBACK = -7
};
enum giulia_pcie_qos_condition {
    GIULIA_QOS_CURRENT_DT_LAYOUT = 1u << 0,
    /* Real provider BCM proxy votes are completed and retained. The HLOS
     * CPU floor or a CRM table ACK alone does not establish this condition. */
    GIULIA_QOS_PROXY_VOTES_COMPLETE = 1u << 1,
    /* GEM provider clock and both NoC configuration access paths available.
     * This core only owns the two PCIe ANOC GCC branch votes below. */
    GIULIA_QOS_GEM_CLOCK_ACCESS_READY = 1u << 2,
    /* Includes other PCIe clients of the shared GEM qnm_pcie port. */
    GIULIA_QOS_SHARED_CLIENTS_QUIET = 1u << 3,
    GIULIA_QOS_GCC_QOS_WRITERS_SERIALIZED = 1u << 4,
    GIULIA_QOS_FIRMWARE_CESTA_COORDINATED = 1u << 5,
    GIULIA_QOS_NO_LOW_POWER_TRANSITION = 1u << 6
};
#define GIULIA_QOS_REQUIRED 0x7fu
struct giulia_pcie_qos_io {
    void *cookie;
    int (*read32)(void *, unsigned block, uint32_t offset, uint32_t *);
    /* Nonzero may still mean a write reached the device; ownership remains. */
    int (*write32)(void *, unsigned block, uint32_t offset, uint32_t);
    void (*barrier)(void *);
    /* Consult the actual resource owners before every MMIO access. Exclude
     * all writers, hold proxy votes/GEM clock and keep clients quiet through
     * stop; no success stub, mapping-only proof or cached Android status. */
    int (*validate_lease)(void *);
    uint32_t phys[3], size[3];
};
struct giulia_pcie_qos {
    struct giulia_pcie_qos_io io;
    uint32_t state, owned, saved[3], valid, dirty;
    uint32_t failed_block, failed_offset, expected, observed, observed_valid;
    int error, cleanup_error, callback_error;
};
/* Zero once, serialize calls and retain stable object address/mappings/leases
 * until STOPPED. Save GCC votes; enable PCIe ANOC aggregate+config clocks;
 * snapshot both QoS words; apply vendor priority forwarding, priority and
 * urgent forwarding fields downstream first. Active readback means these
 * static fields are configured, NOT complete NoC/CESTA/PHY readiness.
 * No reset, SM sequence, CRM table, DMA, link, IRQ or firmware operation.
 * On error attempt rollback; FAULT retains resources for explicit stop retry.
 * There is no automatic XNU boot hook or platform-ownership implementation.
 */
int giulia_pcie_qos_start(struct giulia_pcie_qos *,
    const struct giulia_pcie_qos_io *, uint32_t conditions);
/* Read-only verification under the same quiet lease. It cannot confer a
 * lease or prove that NoC transactions/firmware are operational. */
int giulia_pcie_qos_check(struct giulia_pcie_qos *);
/* All shared users must still be quiet. Restore only touched fields using
 * current RMWs, preserving unrelated bits. Restore QoS before branch votes.
 * A failed restore keeps that field and all lower dependencies owned; retry
 * skips completed fields. No forced-off/HWCG/CBCR write or OFF-state claim. */
int giulia_pcie_qos_stop(struct giulia_pcie_qos *);
#endif
