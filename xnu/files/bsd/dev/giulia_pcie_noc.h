/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_PCIE_NOC_H
#define GIULIA_PCIE_NOC_H

#include <stdint.h>

/* Read-only snapshot of the OnePlus SM8650 PCIe0 CESTA/CRM layout.
 * No clocks, votes, overrides, sequences, interrupts or channels are changed.
 * An OK result means that reads completed, not that NoC is ready, that CESTA
 * has relinquished control, or that the observations form an atomic snapshot.
 */
#define GIULIA_NOC_SM_PHYS 0x01d07000u
#define GIULIA_NOC_SM_SIZE 0x7000u
#define GIULIA_NOC_CRM_PHYS 0x01d01000u
#define GIULIA_NOC_CRM_SIZE 0x3000u
#define GIULIA_NOC_CHANNELS 2u
#define GIULIA_NOC_STATES 5u
#define GIULIA_NOC_MAX_PERF 8u
#define GIULIA_NOC_MAX_BW 16u
#define GIULIA_NOC_SM_REGS 12u
#define GIULIA_NOC_HEADER_REGS 4u
#define GIULIA_NOC_CHANNEL_REGS 2u
#define GIULIA_NOC_SM_START_MASK 1u
#define GIULIA_NOC_CHANNEL_MASK 3u
#define GIULIA_NOC_PERF_VALUE_MASK 7u
#define GIULIA_NOC_BW_VALID (1u << 29)
/* Generic protocol definition, SW DRV only; not a HW DRV0 table field. */
#define GIULIA_NOC_BW_COMMIT (1u << 30)
#define GIULIA_NOC_BW_FIELD_MASK 0x3fffu

enum giulia_pcie_noc_block { GIULIA_NOC_SM, GIULIA_NOC_CRM };
enum giulia_pcie_noc_result {
    GIULIA_NOC_OK = 0, GIULIA_NOC_ARGUMENT = -1,
    GIULIA_NOC_PREREQUISITE = -2, GIULIA_NOC_READ_ERROR = -3,
    GIULIA_NOC_LAYOUT = -4, GIULIA_NOC_DISABLED = -5
};
enum giulia_pcie_noc_prerequisite {
    /* Verified live DT compatible + both physical resources below. */
    GIULIA_NOC_SM8650_PCIE0_LAYOUT = 1u << 0,
    /* Adapter has independently established that both MMIO blocks may be
     * read in the current power/security state. Mapping alone is insufficient;
     * this module cannot catch a hardware abort or enable their access path. */
    GIULIA_NOC_MMIO_READABLE = 1u << 1
};
#define GIULIA_NOC_REQUIRED 3u
enum giulia_pcie_noc_valid {
    GIULIA_NOC_HAVE_SM = 1u << 0, GIULIA_NOC_HAVE_HEADER = 1u << 1,
    GIULIA_NOC_HAVE_CHANNEL = 1u << 2, GIULIA_NOC_HAVE_TABLES = 1u << 3,
    GIULIA_NOC_HAVE_STATUS = 1u << 4, GIULIA_NOC_HAVE_AFTER = 1u << 5,
    /* A before/after register changed. Absence does not rule out an ABA
     * transition or concurrent table edits; never use this as a write lease. */
    GIULIA_NOC_OBSERVED_CHANGE = 1u << 6
};
enum giulia_pcie_noc_sm_index {
    GIULIA_NOC_SM_PWR_OVERRIDE, GIULIA_NOC_SM_PWR_MASK,
    GIULIA_NOC_SM_WAIT_OVERRIDE, GIULIA_NOC_SM_WAIT_MASK,
    GIULIA_NOC_SM_BRANCH_OVERRIDE, GIULIA_NOC_SM_BRANCH_MASK,
    GIULIA_NOC_SM_PWR_STATUS, GIULIA_NOC_SM_WAIT_STATUS,
    GIULIA_NOC_SM_BRANCH_STATUS, GIULIA_NOC_SM_PC,
    GIULIA_NOC_SM_START, GIULIA_NOC_SM_CLKREQ_STATUS
};
enum giulia_pcie_noc_header_index {
    GIULIA_NOC_VERSION, GIULIA_NOC_CFG1, GIULIA_NOC_CFG2,
    GIULIA_NOC_ENABLE
};
enum giulia_pcie_noc_channel_index {
    GIULIA_NOC_CH_BUSY, GIULIA_NOC_CH_UPDATE
};

struct giulia_pcie_noc_io {
    void *cookie;
    /* Return zero on success. Must perform an ordered device read; there is
     * deliberately no write callback. Do not turn clocks on in this callback. */
    int (*read32)(void *, unsigned block, uint32_t offset, uint32_t *value);
    uint32_t sm_phys, sm_size, crm_phys, crm_size;
};

struct giulia_pcie_noc_snapshot {
    uint32_t valid, read_count, failed_block, failed_offset;
    int callback_error;
    uint32_t sm[GIULIA_NOC_SM_REGS], sm_after[GIULIA_NOC_SM_REGS];
    uint32_t header[GIULIA_NOC_HEADER_REGS];
    uint32_t header_after[GIULIA_NOC_HEADER_REGS];
    uint32_t channel[GIULIA_NOC_CHANNEL_REGS];
    uint32_t channel_after[GIULIA_NOC_CHANNEL_REGS];
    /* Version is informational: vendor qcom,pcie-crm uses this layout without
     * a version switch. No measured silicon version is invented here. */
    uint32_t major, minor, hw_drvs, sw_drvs, rails, bw_vcds;
    uint32_t channels, states, perf_resources, bw_resources;
    /* HW DRV 0 only; every entry is raw, with [channel][state][resource].
     * Zero entries beyond the reported resource counts are NOT observations. */
    uint32_t perf[GIULIA_NOC_CHANNELS][GIULIA_NOC_STATES][GIULIA_NOC_MAX_PERF];
    uint32_t bw[GIULIA_NOC_CHANNELS][GIULIA_NOC_STATES][GIULIA_NOC_MAX_BW];
    uint32_t perf_status[GIULIA_NOC_MAX_PERF], pwr_idx_status;
};

/* Caller supplies private storage, serializes its own requests, and retains
 * accessible mappings throughout. No allocation, delay, polling or retry.
 * On failure, valid identifies only the completely captured sections. */
int giulia_pcie_noc_snapshot(const struct giulia_pcie_noc_io *,
    uint32_t prerequisites, struct giulia_pcie_noc_snapshot *);

#endif
