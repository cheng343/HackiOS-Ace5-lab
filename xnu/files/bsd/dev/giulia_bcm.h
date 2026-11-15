/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_BCM_H
#define GIULIA_BCM_H
#include "giulia_crm.h"
enum giulia_bcm_result { GIULIA_BCM_ARGUMENT = -20, GIULIA_BCM_DATABASE = -21,
    GIULIA_BCM_NOT_FOUND = -22, GIULIA_BCM_OVERFLOW = -23 };
struct giulia_bcm_resource { uint32_t address,unit,width,vcd; };
struct giulia_bcm_vote { uint32_t x,y,command,saturated; };
int giulia_bcm_resolve(const uint8_t *, uint32_t bytes, uint32_t expected_version,
    const char *name, struct giulia_bcm_resource *);
/* Match vendor bcm_aggregate/crm_cmd_gen: units are kB/s; two integer
 * divisions truncate with nonzero-minimum1, NOT round-up. Scale=1000.
 * enable_mask is 0 (numeric) or 1 (board's SH1/ACV on/off encoding).
 * Overflow is rejected; representable votes saturate at14bits and report it.
 * command always has valid+commit, as the vendor CRM path does, even at0.
 */
int giulia_bcm_encode(const struct giulia_bcm_resource *, uint64_t avg_kbps,
    uint64_t peak_kbps, uint32_t buswidth, uint32_t channels,
    uint32_t enable_mask, struct giulia_bcm_vote *);
enum giulia_bcm_pcie0_policy {
    /* DT omits qcom,no-client-based-bw-voting: vendor's actual default. */
    GIULIA_BCM_PCIE0_FIXED_500_800,
    /* Use only when current DT explicitly selects rate-based ICC voting. */
    GIULIA_BCM_PCIE0_GEN1_X1_250000_0
};
#define GIULIA_BCM_PCIE0_PATCHES 12u
struct giulia_bcm_pcie0_plan {
    struct giulia_crm_patch patch[12];
    struct giulia_bcm_resource resource[5];
    struct giulia_bcm_vote vote[5];
};
/* OnePlus pineapple PCIe0 CRM HW DRV0 data path, Gen1x1 only. Five BCMs:
 * MC0/SH0/SH1/SN4/ACV at CRM nodes0/1/2/3/5. IP0/node4 and nodes6..15
 * are never patched. PERF resource0 and BW votes affect only states0/1.
 * Every address/unit/width comes from this boot's CMD DB, not a static dump.
 * Caller matches the path/CRM client to the live DT and selects its policy.
 * Success creates a plan, NOT a ready NoC: CPU->PCIe/CN0, QoS, current SM
 * state, physical completion, firmware and endpoint remain outside it.
 */
int giulia_bcm_pcie0_plan(const uint8_t *, uint32_t bytes, uint32_t version,
    unsigned policy, struct giulia_bcm_pcie0_plan *);
/* Same planning, then actual production CRM table/channel transfer under
 * its real platform lease. Inspect CRM ownership/dirty/pending on failure. */
int giulia_bcm_pcie0_start(struct giulia_crm *, const struct giulia_crm_io *,
    const uint8_t *, uint32_t bytes, uint32_t version, unsigned policy,
    struct giulia_bcm_pcie0_plan *);
/* Explicit empty-channel initialization with retained lifetime. Never auto
 * selected after the ordinary active-channel API rejects an empty device. */
int giulia_bcm_pcie0_start_empty_keep(struct giulia_crm *, const struct giulia_crm_io *,
    const uint8_t *, uint32_t bytes, uint32_t version, unsigned policy,
    struct giulia_bcm_pcie0_plan *);
#endif
