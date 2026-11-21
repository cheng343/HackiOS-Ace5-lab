/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_PCIE_POWER_H
#define GIULIA_PCIE_POWER_H

#include <stdint.h>

/* SM8650 PCIe0 only. No mapping, global instance, boot hook or firmware votes.
 * The caller serializes all operations, including shared GCC register RMWs.
 * MMIO must remain accessible until stop succeeds, including after a timeout.
 */
#define GIULIA_POWER_GCC_PHYS 0x00100000u
#define GIULIA_POWER_TCSR_PHYS 0x01fc0000u
enum giulia_pcie_power_block { GIULIA_POWER_GCC, GIULIA_POWER_TCSR };
enum giulia_pcie_power_result {
    GIULIA_POWER_OK = 0, GIULIA_POWER_ARGUMENT = -1,
    GIULIA_POWER_PREREQUISITE = -2, GIULIA_POWER_STATE = -3,
    GIULIA_POWER_BUSY = -4, GIULIA_POWER_READBACK = -5,
    GIULIA_POWER_TIMEOUT = -6
};
enum giulia_pcie_power_state {
    GIULIA_POWER_IDLE = 0, GIULIA_POWER_READY, GIULIA_POWER_PIPE_ON,
    GIULIA_POWER_STOPPED, GIULIA_POWER_FAULT, GIULIA_POWER_HOST_PREPARED
};
enum giulia_pcie_power_prerequisite {
    GIULIA_POWER_MMIO_OWNED = 1u << 0,
    GIULIA_POWER_PCIE0_EXCLUSIVE = 1u << 1,
    GIULIA_POWER_RAILS_READY = 1u << 2,
    GIULIA_POWER_NOC_READY = 1u << 3,
    GIULIA_POWER_DIRECT_VOTES = 1u << 4,
    GIULIA_POWER_XO_19M2_GPLL0_EVEN_300M = 1u << 5,
    GIULIA_POWER_ENDPOINT_RESET = 1u << 6,
    GIULIA_POWER_LINK_DMA_QUIET = 1u << 7,
    /* Optional platform handoff of an inherited PCIe0-only RCHNG vote.
     * Requires all ordinary prerequisites AND confirmed relinquishment by
     * every previous clock user. The core independently checks both domains
     * are collapsed/down and every other PCIe leaf is disabled before writes.
     * This flag never follows merely from reading the inherited vote. */
    GIULIA_POWER_RCHNG_HANDOFF = 1u << 8,
    /* Platform's actual CESTA owner permits this exclusive software GCC vote
     * sequence and retains coordination through withdrawal. It need NOT halt
     * CESTA. This is not implied by START=0 or a CRM channel ACK. Mutually
     * exclusive with DIRECT_VOTES. Giulia's opt-in XNU Wi-Fi supply and
     * bringup path uses this managed route. */
    GIULIA_POWER_CESTA_COORDINATED = 1u << 9
};
#define GIULIA_POWER_REQUIRED 0xffu
#define GIULIA_POWER_HOST_REQUIRED (GIULIA_POWER_MMIO_OWNED | \
    GIULIA_POWER_PCIE0_EXCLUSIVE | GIULIA_POWER_RAILS_READY | \
    GIULIA_POWER_XO_19M2_GPLL0_EVEN_300M | GIULIA_POWER_ENDPOINT_RESET | \
    GIULIA_POWER_LINK_DMA_QUIET)
enum giulia_pcie_power_stop_condition {
    GIULIA_POWER_STOP_ENDPOINT_RESET = 1u << 0,
    GIULIA_POWER_STOP_LINK_DMA_QUIET = 1u << 1,
    GIULIA_POWER_STOP_PHY_QUIET = 1u << 2,
    /* Coordinated route only: platform has withdrawn/quiesced its managed
     * CESTA resource operation, or has never submitted it. Keep the GCC host
     * clocks until that owning service completes; a timeout is not withdrawal. */
    GIULIA_POWER_STOP_CESTA_RESOURCES_QUIET = 1u << 3
};
#define GIULIA_POWER_STOP_REQUIRED 7u

struct giulia_pcie_power_io {
    void *cookie;
    uint32_t (*read32)(void *, unsigned block, uint32_t offset);
    void (*write32)(void *, unsigned block, uint32_t offset, uint32_t value);
    void (*barrier)(void *);
    void (*delay_us)(void *, unsigned);
};

/* Private saved register fields; do not modify an active object. */
#define GIULIA_POWER_SAVED_REGS 13u
struct giulia_pcie_power {
    struct giulia_pcie_power_io io;
    uint32_t state, dirty, saved[GIULIA_POWER_SAVED_REGS];
    uint32_t inherited_rchng, prerequisites;
    /* Set before the first BCR write; remains set on a failed finish/stop.
     * Only a new admitted prepare clears it. Phase-one cancel may not treat
     * a failed later reset as an untouched HOST_PREPARED operation. */
    uint32_t reset_attempted;
    int last_error, cleanup_error;
};

/* Zero-initialize once. Missing flags cause no MMIO. All flags need actual
 * platform evidence: RAILS_READY includes PHY/endpoint supplies and AOP/RPMh
 * votes; NOC_READY includes CPU access and PCIe data paths/QoS, not just clock
 * gates. DIRECT_VOTES means vendor CESTA/firmware no longer owns this sequence.
 * This module neither proves those facts nor changes shared PLL frequencies.
 * It refuses live PCIe0 branch votes, an RCG update in progress or GDSC HW mode.
 * RCHNG_HANDOFF alone permits the inherited RCHNG branch after cold-state
 * admission. Its vote remains asserted throughout start/rollback/stop and its
 * original RCG fields are restored; no shared PLL is reprogrammed. This is not
 * a running-link handoff or permission to take a clock from firmware.
 * start powers domains, enables host clocks, pulses core then PHY BCR and
 * enables PHY aux/ref/rchng resources. PIPE stays disabled on the XO mux.
 * CORE BCR is held 1000 us, then settled 1000 us. The direct QMP profile
 * holds PHY BCR for 200 us; the giulia CESTA path uses the stock 1000 us.
 * The caller may then program the QMP PHY while holding endpoint PERST#.
 * A failed start attempts rollback; inspect state/cleanup_error. A FAULT must
 * be stopped successfully before releasing mappings/rails/NoC ownership.
 */
int giulia_pcie_power_start(struct giulia_pcie_power *,
    const struct giulia_pcie_power_io *, uint32_t prerequisites);
/* Split vendor sequence: GDSCs/host clocks -> external CESTA resource request
 * -> core/PHY reset. prepare_host touches only GCC/TCSR; it never pulses BCR
 * or enables PIPE. Requires HOST_REQUIRED and exactly one ownership route.
 * The direct route also requires NOC_READY at entry, as the old start does.
 * The coordinated route allows the NoC resource service to run in between,
 * matching fixed vendor pci-msm.c:msm_pcie_enable. The service itself is NOT
 * implemented by these APIs; only its real owner may establish these flags.
 */
int giulia_pcie_power_prepare_host(struct giulia_pcie_power *,
    const struct giulia_pcie_power_io *, uint32_t prerequisites);
/* Requires the SAME original leases plus actual NOC_READY, including CPU
 * access/data-path/QoS readiness, not just staged votes or a channel ACK.
 * Missing conditions do no MMIO and leave HOST_PREPARED for cancel/retry.
 * On a coordinated reset failure, retain clocks/snapshots in FAULT: the
 * external resource service must finish withdrawal before power_stop.
 * The legacy direct start retains its automatic rollback behavior.
 */
int giulia_pcie_power_finish_reset(struct giulia_pcie_power *, uint32_t prerequisites);
/* QMP callbacks: enable after table programming, before SW_RESET/start.
 * disable after link/DMA quiescence + endpoint reset, before PHY SW_RESET /
 * START / POWER_DOWN, matching qmp_pcie_power_off. Other clocks stay enabled.
 * The caller, not this API, supplies and verifies those external conditions.
 */
int giulia_pcie_power_pipe_enable(struct giulia_pcie_power *);
/* Poll GCC_PCIE_0_PIPE_CBCR (0x6b040) after QMP PHY_START until CLK_OFF clears
 * (or FSM reports ON). Read-only: does not mutate power state on timeout. */
int giulia_pcie_power_pipe_ready(struct giulia_pcie_power *, uint32_t *observed_cbcr);
int giulia_pcie_power_pipe_disable(struct giulia_pcie_power *);
/* PHY_QUIET means PHY stop completed while aux/cfg/ref/rchng were still on.
 * Restores owned fields, never unrelated/shared vote bits or status fields.
 * Votable GDSC withdrawal waits 500 us without requiring power OFF: another
 * master can keep a domain on. Failure preserves snapshots for a stop retry.
 * A successful stop restores GCC/TCSR state, not destroyed PCIe/PHY contents.
 */
int giulia_pcie_power_stop(struct giulia_pcie_power *, uint32_t conditions);

#endif
