/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_WIFI_BRINGUP_H
#define GIULIA_WIFI_BRINGUP_H

#include "giulia_wifi_supply.h"
#include "giulia_pcie_link.h"
#include "giulia_pcie_phy.h"
#include "giulia_kiwi_reg.h"
#include "giulia_mhi_snapshot.h"

/* End-to-end serial bring-up coordinator from WLAN supply/AOP/PDC/NoC
 * through PCIe0 PHY/link/BAR0 to WCN7850 SoC identity and MHI snapshot. */
enum giulia_wifi_bringup_result {
    GIULIA_BRINGUP_OK = 0,
    GIULIA_BRINGUP_RETAINED = 1,
    GIULIA_BRINGUP_ARGUMENT = -1,
    GIULIA_BRINGUP_STATE = -2,
    GIULIA_BRINGUP_PREREQUISITE = -3,
    GIULIA_BRINGUP_SUPPLY = -4,
    GIULIA_BRINGUP_POWER = -5,
    GIULIA_BRINGUP_LINK = -6,
    GIULIA_BRINGUP_PHY = -7,
    GIULIA_BRINGUP_GPIO = -8,
    GIULIA_BRINGUP_BAR = -9,
    GIULIA_BRINGUP_MHI = -10
};

enum giulia_wifi_bringup_stage {
    GIULIA_BRINGUP_IDLE = 0,
    GIULIA_BRINGUP_SUPPLY_STAGE,
    GIULIA_BRINGUP_NOC_STAGE,
    GIULIA_BRINGUP_POWER_RESET,
    GIULIA_BRINGUP_PHY_START,
    GIULIA_BRINGUP_LINK_PREPARE,
    GIULIA_BRINGUP_PERST_RELEASE,
    GIULIA_BRINGUP_LINK_TRAIN,
    GIULIA_BRINGUP_MEM_ENABLE,
    GIULIA_BRINGUP_SOC_INFO,
    GIULIA_BRINGUP_MHI_SNAP,
    GIULIA_BRINGUP_READY,
    GIULIA_BRINGUP_STOPPING,
    GIULIA_BRINGUP_STOPPED,
    GIULIA_BRINGUP_HELD,
    GIULIA_BRINGUP_FAULT
};

struct giulia_wifi_bringup_config {
    struct giulia_wifi_supply_config supply;
    struct giulia_pcie_link_io link;
    struct giulia_pcie_phy_io phy; /* pipe_enable/disable bound internally */
    struct giulia_kiwi_bar_io bar;
    struct giulia_mhi_snapshot_io mhi;
    void *noc_cookie;
    /* Optional hook invoked after HOST_PREPARED (e.g. CESTA/CRM/QoS setup)
     * before finishing PCIe BCR reset. Zero means NoC/QoS is ready. */
    int (*prepare_noc)(void *cookie, struct giulia_wifi_supply *supply);
    /* Runs while host clocks/domains are still owned, after link/PHY stop.
     * Restore QoS before its clocks. Return OK only after managed resources
     * are withdrawn; RETAINED keeps host power, supplies and platform leases
     * until cold reset. A negative result keeps all lower dependencies. */
    int (*release_noc)(void *cookie);
    int (*verify_noc)(void *cookie);
    /* Optional diagnostic stop ceiling (enum giulia_wifi_bringup_stage, 0 = full). */
    unsigned max_stage;
};

struct giulia_wifi_bringup {
    struct giulia_wifi_bringup_config cfg;
    struct giulia_wifi_supply supply;
    struct giulia_pcie_link link;
    struct giulia_pcie_phy phy;
    struct giulia_kiwi_soc_info soc;
    struct giulia_mhi_snapshot mhi;
    uint32_t stage, failed_stage, noc_started;
    uint32_t pipe_checked, pipe_cbcr;
    int error, sub_error, cleanup_error;
};

int giulia_wifi_bringup_start(struct giulia_wifi_bringup *b,
    const struct giulia_wifi_bringup_config *cfg);
int giulia_wifi_bringup_stop(struct giulia_wifi_bringup *b);

#endif
