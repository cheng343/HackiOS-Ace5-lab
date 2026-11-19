/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_PCIE_PROFILE_H
#define GIULIA_PCIE_PROFILE_H

/* Initial CPU-only bringup. The CRM planner and RC must use the same ceiling.
 * Higher rates require a separately completed resource/clock transition;
 * this is not the controller's or WCN7850's maximum hardware capability.
 */
#define GIULIA_PCIE_BOOT_SPEED 1u
#define GIULIA_PCIE_BOOT_LANES 1u
#define GIULIA_PCIE_BOOT_PERF 2u
/* pci-msm.c: D0 requests PERF_LVL_L1SS (1) + the three RC0 bw-scale
 * rows in the verified giulia DT before it negotiates the link speed. */
#define GIULIA_PCIE_STOCK_STARTUP_PERF 4u

#endif
