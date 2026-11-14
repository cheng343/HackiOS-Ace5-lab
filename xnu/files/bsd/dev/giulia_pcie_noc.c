/* SPDX-License-Identifier: BSD-2-Clause */
#include "giulia_pcie_noc.h"

/* Independently implemented read-only diagnostic. Register facts checked at:
 * OnePlusOSS/android_kernel_oneplus_sm8650, commit
 * 66305c56af1fcd7299c906c3257e585be93d93ed:
 * drivers/soc/qcom/crm.c:38-80,162-218,400-427,580-620,1236-1283,1484-1515;
 * drivers/pci/controller/pci-msm.c:1804-1820 (pcie_sm_dump),3791-3824.
 * SM offsets/resources independently matched to the device's pineapple DT.
 * https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/blob/
 * 66305c56af1fcd7299c906c3257e585be93d93ed/drivers/soc/qcom/crm.c
 * These reads are used by vendor diagnostics, but access while the relevant
 * power/security path is unavailable remains the adapter's responsibility.
 */
static const uint32_t sm_offsets[GIULIA_NOC_SM_REGS] = {
    0x1040u, 0x1048u, 0x1050u, 0x1058u, 0x1060u, 0x1068u,
    0x1070u, 0x1078u, 0x1080u, 0x1088u, 0x1090u, 0x109cu
};
/* CLKREQ_GATE/UNGATE command offsets 0x1094/0x1098 are intentionally omitted. */
static const uint32_t channel_offsets[GIULIA_NOC_CHANNEL_REGS] = {
    0x1000u, 0x1020u
};
/* CHN_BEHAVE is omitted: vendor PCIe descriptor disables its programming,
 * and the vendor diagnostic does not read it. */
static const uint32_t perf_state_offsets[GIULIA_NOC_STATES] = {
    0x000u, 0x040u, 0x080u, 0x600u, 0x640u
};
static const uint32_t bw_state_offsets[GIULIA_NOC_STATES] = {
    0x0c0u, 0x140u, 0x680u, 0x700u, 0x780u
};

static int read_reg(const struct giulia_pcie_noc_io *io,
    struct giulia_pcie_noc_snapshot *s, unsigned block, uint32_t offset,
    uint32_t *value)
{
    uint32_t size = block == GIULIA_NOC_SM ? io->sm_size : io->crm_size;
    int err;
    if ((offset & 3u) || size < 4u || offset > size - 4u)
        return GIULIA_NOC_LAYOUT;
    s->read_count++;
    err = io->read32(io->cookie, block, offset, value);
    if (!err)
        return GIULIA_NOC_OK;
    s->failed_block = block;
    s->failed_offset = offset;
    s->callback_error = err;
    return GIULIA_NOC_READ_ERROR;
}

static int read_sm(const struct giulia_pcie_noc_io *io,
    struct giulia_pcie_noc_snapshot *s, uint32_t *dest)
{
    unsigned i;
    int err;
    for (i = 0; i < GIULIA_NOC_SM_REGS; i++) {
        err = read_reg(io, s, GIULIA_NOC_SM, sm_offsets[i], &dest[i]);
        if (err) return err;
    }
    return GIULIA_NOC_OK;
}

static int read_header(const struct giulia_pcie_noc_io *io,
    struct giulia_pcie_noc_snapshot *s, uint32_t *dest)
{
    unsigned i;
    int err;
    for (i = 0; i < GIULIA_NOC_HEADER_REGS; i++) {
        err = read_reg(io, s, GIULIA_NOC_CRM, i * 4u, &dest[i]);
        if (err) return err;
    }
    return GIULIA_NOC_OK;
}

static int read_channel(const struct giulia_pcie_noc_io *io,
    struct giulia_pcie_noc_snapshot *s, uint32_t *dest)
{
    unsigned i;
    int err;
    for (i = 0; i < GIULIA_NOC_CHANNEL_REGS; i++) {
        err = read_reg(io, s, GIULIA_NOC_CRM, channel_offsets[i], &dest[i]);
        if (err) return err;
    }
    return GIULIA_NOC_OK;
}

static int decode_layout(struct giulia_pcie_noc_snapshot *s)
{
    uint32_t cfg = s->header[GIULIA_NOC_CFG1];
    s->major = (s->header[GIULIA_NOC_VERSION] >> 16) & 0xffu;
    s->minor = (s->header[GIULIA_NOC_VERSION] >> 8) & 0xffu;
    s->hw_drvs = (cfg >> 16) & 15u;
    s->sw_drvs = (cfg >> 20) & 15u;
    s->rails = (cfg >> 12) & 15u;
    s->bw_vcds = (cfg >> 24) & 15u;
    s->channels = (cfg >> 4) & 15u;
    s->states = cfg & 15u;
    s->perf_resources = (cfg >> 8) & 15u;
    /* BW table channel stride comes from CFG2's node count, NOT BW VCDs. */
    s->bw_resources = (s->header[GIULIA_NOC_CFG2] >> 26) & 31u;
    if (s->header[GIULIA_NOC_VERSION] == 0xffffffffu ||
        s->header[GIULIA_NOC_ENABLE] == 0xffffffffu ||
        !s->hw_drvs || s->channels != GIULIA_NOC_CHANNELS ||
        s->states != GIULIA_NOC_STATES || !s->perf_resources ||
        s->perf_resources > GIULIA_NOC_MAX_PERF || !s->bw_resources ||
        s->bw_resources > GIULIA_NOC_MAX_BW)
        return GIULIA_NOC_LAYOUT;
    /* Limits keep both channels inside the documented per-state slot.
     * They are bounds, not a claim that hardware has 8 PERF/16 BW resources. */
    if (!s->header[GIULIA_NOC_ENABLE])
        return GIULIA_NOC_DISABLED;
    return GIULIA_NOC_OK;
}

int giulia_pcie_noc_snapshot(const struct giulia_pcie_noc_io *io,
    uint32_t prerequisites, struct giulia_pcie_noc_snapshot *s)
{
    unsigned i, ch, st, res;
    uint32_t offset;
    int err;
    if (!s) return GIULIA_NOC_ARGUMENT;
    *s = (struct giulia_pcie_noc_snapshot){0};
    if (!io || !io->read32) return GIULIA_NOC_ARGUMENT;
    if ((prerequisites & GIULIA_NOC_REQUIRED) != GIULIA_NOC_REQUIRED)
        return GIULIA_NOC_PREREQUISITE;
    if (io->sm_phys != GIULIA_NOC_SM_PHYS ||
        io->sm_size != GIULIA_NOC_SM_SIZE ||
        io->crm_phys != GIULIA_NOC_CRM_PHYS ||
        io->crm_size != GIULIA_NOC_CRM_SIZE)
        return GIULIA_NOC_LAYOUT;
    err = read_sm(io, s, s->sm);
    if (err) return err;
    s->valid |= GIULIA_NOC_HAVE_SM;
    err = read_header(io, s, s->header);
    if (err) return err;
    s->valid |= GIULIA_NOC_HAVE_HEADER;
    err = decode_layout(s);
    if (err) return err;
    err = read_channel(io, s, s->channel);
    if (err) return err;
    s->valid |= GIULIA_NOC_HAVE_CHANNEL;
    for (ch = 0; ch < s->channels; ch++) {
        for (st = 0; st < s->states; st++) {
            for (res = 0; res < s->perf_resources; res++) {
                offset = 0x200u + perf_state_offsets[st] +
                    4u * (ch * s->perf_resources + res);
                err = read_reg(io, s, GIULIA_NOC_CRM, offset,
                    &s->perf[ch][st][res]);
                if (err) return err;
            }
            for (res = 0; res < s->bw_resources; res++) {
                offset = 0x200u + bw_state_offsets[st] +
                    4u * (ch * s->bw_resources + res);
                err = read_reg(io, s, GIULIA_NOC_CRM, offset,
                    &s->bw[ch][st][res]);
                if (err) return err;
            }
        }
    }
    s->valid |= GIULIA_NOC_HAVE_TABLES;
    for (res = 0; res < s->perf_resources; res++) {
        err = read_reg(io, s, GIULIA_NOC_CRM, 0x3c0u + 4u * res,
            &s->perf_status[res]);
        if (err) return err;
    }
    err = read_reg(io, s, GIULIA_NOC_CRM, 0x3f0u, &s->pwr_idx_status);
    if (err) return err;
    s->valid |= GIULIA_NOC_HAVE_STATUS;
    err = read_channel(io, s, s->channel_after);
    if (err) return err;
    err = read_header(io, s, s->header_after);
    if (err) return err;
    err = read_sm(io, s, s->sm_after);
    if (err) return err;
    s->valid |= GIULIA_NOC_HAVE_AFTER;
    for (i = 0; i < GIULIA_NOC_SM_REGS; i++)
        if (s->sm[i] != s->sm_after[i])
            s->valid |= GIULIA_NOC_OBSERVED_CHANGE;
    for (i = 0; i < GIULIA_NOC_HEADER_REGS; i++)
        if (s->header[i] != s->header_after[i])
            s->valid |= GIULIA_NOC_OBSERVED_CHANGE;
    for (i = 0; i < GIULIA_NOC_CHANNEL_REGS; i++)
        if (s->channel[i] != s->channel_after[i])
            s->valid |= GIULIA_NOC_OBSERVED_CHANGE;
    return GIULIA_NOC_OK;
}
