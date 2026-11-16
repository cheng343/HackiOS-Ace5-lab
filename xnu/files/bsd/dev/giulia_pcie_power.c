/* SPDX-License-Identifier: BSD-2-Clause */
/* Original, callback-based SM8650 PCIe0 power sequencer. Register facts and
 * sequencing were checked against these fixed official Linux v6.12 sources:
 * https://github.com/torvalds/linux/blob/v6.12/drivers/clk/qcom/gcc-sm8650.c
 *   gcc_pcie_0_*, gcc_{cnoc,aggre_noc,ddrss}_pcie_*, pcie_0{,_phy}_gdsc,
 *   gcc_sm8650_resets (registers, bits, source selectors, rates, halt policy).
 * https://github.com/torvalds/linux/blob/v6.12/drivers/clk/qcom/gdsc.c
 *   gdsc_init/enable/toggle_logic (wait fields, CFG status, retain, vote delay).
 * https://github.com/torvalds/linux/blob/v6.12/drivers/clk/qcom/clk-rcg2.c
 *   clk_rcg2_shared_enable, update_config, __clk_rcg2_configure.
 * https://github.com/torvalds/linux/blob/v6.12/drivers/clk/qcom/clk-branch.c
 *   clk_branch_wait/clk_branch2_check_halt (skip/HWCG, ON FSM, voted delay).
 * https://github.com/torvalds/linux/blob/v6.12/drivers/clk/qcom/clk-regmap-phy-mux.c
 *   clk_regmap_phy_mux_enable/disable (PHY=0, reference=2, two-bit mux).
 * https://github.com/torvalds/linux/blob/v6.12/drivers/clk/qcom/tcsrcc-sm8650.c
 *   tcsr_pcie_0_clkref_en (offset 0x31100, bit 0, delay halt policy).
 * https://github.com/torvalds/linux/blob/v6.12/arch/arm64/boot/dts/qcom/sm8650.dtsi
 *   pcie0/pcie0_phy/gcc/tcsr (bases, host/PHY clocks and reset assignments).
 * https://github.com/torvalds/linux/blob/v6.12/drivers/pci/controller/dwc/pcie-qcom.c
 *   qcom_pcie_init_2_7_0 (host clocks before CORE BCR, 1 ms on both edges).
 * https://github.com/torvalds/linux/blob/v6.12/drivers/phy/qualcomm/phy-qcom-qmp-pcie.c
 *   qmp_pcie_init/power_on/power_off (200 us PHY BCR, separate PIPE phase).
 * The split host/resource/reset ordering additionally follows fixed vendor:
 * https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/blob/66305c56af1fcd7299c906c3257e585be93d93ed/drivers/pci/controller/pci-msm.c
 *   msm_pcie_enable: vreg -> GDSC -> clocks -> CESTA -> core/PHY reset.
 * This is not a copy of Linux's clock framework. Restoration, ownership,
 * failure states and callback API are local implementation decisions. No
 * CESTA, sheng/SM8750, shared PLL, AOP, rail, NoC QoS or GPIO writes occur.
 */
#include "giulia_pcie_power.h"

#define B(n) (1u << (n))
#define GDSC_FIELDS 0x00fff806u
#define GDSC_INIT_FIELDS 0x00fff006u
#define GDSC_WAIT_DEFAULT 0x00282000u
#define RCG_FIELDS 0x0010371fu
#define ROOT_FORCE B(1)
#define ROOT_UPDATE B(0)
#define ROOT_OFF B(31)

enum saved_index {
    CORE_GDSC, PHY_GDSC, COLLAPSE, VOTES0, VOTES1, REF,
    PIPE_MUX, AUX_CFG, AUX_CMD, RCH_CFG, RCH_CMD, CORE_RESET, PHY_RESET
};
struct power_reg { unsigned block; uint32_t offset, mask; };
static const struct power_reg regs[GIULIA_POWER_SAVED_REGS] = {
    { GIULIA_POWER_GCC, 0x6b004, GDSC_FIELDS },
    { GIULIA_POWER_GCC, 0x6c000, GDSC_FIELDS },
    { GIULIA_POWER_GCC, 0x5214c, B(0) | B(3) },
    { GIULIA_POWER_GCC, 0x52000, B(12) | B(19) | B(22) },
    { GIULIA_POWER_GCC, 0x52008, 0x7fu },
    { GIULIA_POWER_TCSR, 0x31100, B(0) },
    { GIULIA_POWER_GCC, 0x6b070, 3u },
    { GIULIA_POWER_GCC, 0x6b078, RCG_FIELDS },
    { GIULIA_POWER_GCC, 0x6b074, ROOT_FORCE },
    { GIULIA_POWER_GCC, 0x6b05c, RCG_FIELDS },
    { GIULIA_POWER_GCC, 0x6b058, ROOT_FORCE },
    { GIULIA_POWER_GCC, 0x6b000, B(0) },
    { GIULIA_POWER_GCC, 0x6c01c, B(0) }
};

struct branch { unsigned vote; uint32_t bit, cbcr; unsigned skip, hwcg; };
/* Host order follows the pcie0 clock list in sm8650.dtsi; PHY RCHNG is last.
 * BRANCH_HALT_SKIP and HWCG are intentional: an idle hardware-gated clock
 * need not report running. Never modify the HWCG bit to fake readiness.
 */
static const struct branch branches[] = {
    { VOTES1, B(3), 0x6b03c, 0, 0 }, /* aux */
    { VOTES1, B(2), 0x6b038, 0, 1 }, /* cfg */
    { VOTES1, B(1), 0x6b02c, 1, 1 }, /* master AXI */
    { VOTES1, B(0), 0x6b020, 0, 1 }, /* slave AXI */
    { VOTES1, B(5), 0x6b01c, 0, 0 }, /* Q2A */
    { VOTES0, B(19), 0x10074, 1, 1 }, /* DDRSS SF QTB */
    { VOTES0, B(12), 0x10064, 1, 1 }, /* aggregate NoC */
    { VOTES1, B(6), 0x10058, 0, 1 }, /* CNOC SF AXI */
    { VOTES0, B(22), 0x6b054, 0, 0 } /* PHY RCHNG */
};

static uint32_t
read_reg(struct giulia_pcie_power *p, unsigned i)
{
    return p->io.read32(p->io.cookie, regs[i].block, regs[i].offset);
}

static void
delay(struct giulia_pcie_power *p, unsigned us)
{
    p->io.delay_us(p->io.cookie, us);
}

static int
change(struct giulia_pcie_power *p, unsigned i, uint32_t mask, uint32_t value)
{
    uint32_t old = read_reg(p, i);
    uint32_t got;
    if (old == 0xffffffffu)
        return GIULIA_POWER_READBACK;
    p->dirty |= B(i); /* Also own an attempted write with a failed readback. */
    p->io.write32(p->io.cookie, regs[i].block, regs[i].offset,
        (old & ~mask) | (value & mask));
    p->io.barrier(p->io.cookie);
    got = read_reg(p, i);
    return got != 0xffffffffu && (got & mask) == (value & mask) ?
        GIULIA_POWER_OK : GIULIA_POWER_READBACK;
}

static int
poll_gcc(struct giulia_pcie_power *p, uint32_t offset, uint32_t mask,
    uint32_t value, unsigned limit_us)
{
    unsigned n;
    uint32_t got;
    for (n = 0; n < limit_us; n++) {
        got = p->io.read32(p->io.cookie, GIULIA_POWER_GCC, offset);
        if (got == 0xffffffffu)
            return GIULIA_POWER_READBACK;
        if ((got & mask) == value)
            return GIULIA_POWER_OK;
        delay(p, 1);
    }
    return GIULIA_POWER_TIMEOUT;
}

static int
gdsc_on(struct giulia_pcie_power *p, unsigned i, uint32_t vote)
{
    int r;
    if ((r = change(p, i, GDSC_INIT_FIELDS, GDSC_WAIT_DEFAULT)) ||
        (r = change(p, COLLAPSE, vote, 0)) ||
        (r = poll_gcc(p, regs[i].offset + 4, B(16), B(16), 1500)))
        return r;
    delay(p, 1);
    /* RETAIN_FF is in GDSCR, not the adjacent CFG status register. */
    return change(p, i, B(11), B(11));
}

static int
rcg_apply(struct giulia_pcie_power *p, unsigned cmd, unsigned cfg,
    uint32_t value, uint32_t final_force)
{
    int r;
    uint32_t v;
    /* Retry cleanup only after an older command has finished. Never clear a
     * live UPDATE bit in software and pretend its shadow CFG was committed.
     */
    if ((r = poll_gcc(p, regs[cmd].offset, ROOT_UPDATE, 0, 500)) ||
        (r = change(p, cmd, ROOT_FORCE, ROOT_FORCE)) ||
        (r = poll_gcc(p, regs[cmd].offset, ROOT_OFF, 0, 500)) ||
        (r = change(p, cfg, RCG_FIELDS, value)))
        return r;
    v = read_reg(p, cmd);
    if (v == 0xffffffffu)
        return GIULIA_POWER_READBACK;
    p->dirty |= B(cmd);
    p->io.write32(p->io.cookie, GIULIA_POWER_GCC, regs[cmd].offset,
        v | ROOT_UPDATE);
    p->io.barrier(p->io.cookie);
    if ((r = poll_gcc(p, regs[cmd].offset, ROOT_UPDATE, 0, 500)))
        return r;
    v = read_reg(p, cfg);
    if (v == 0xffffffffu || (v & RCG_FIELDS) != (value & RCG_FIELDS))
        return GIULIA_POWER_READBACK;
    /* Preserve an inherited force vote; Linux's clock framework instead
     * clears its own force vote unconditionally after its transaction.
     */
    return change(p, cmd, ROOT_FORCE, final_force);
}

static int
branch_on(struct giulia_pcie_power *p, const struct branch *b)
{
    unsigned n;
    uint32_t v;
    int r = change(p, b->vote, b->bit, b->bit);
    if (r || b->skip)
        return r;
    v = p->io.read32(p->io.cookie, GIULIA_POWER_GCC, b->cbcr);
    if (v == 0xffffffffu)
        return GIULIA_POWER_READBACK;
    if (b->hwcg && (v & B(1)))
        return GIULIA_POWER_OK;
    for (n = 0; n < 200; n++) {
        v = p->io.read32(p->io.cookie, GIULIA_POWER_GCC, b->cbcr);
        if (v == 0xffffffffu)
            return GIULIA_POWER_READBACK;
        if (!(v & B(31)) || ((v >> 28) & 7u) == 2u)
            return GIULIA_POWER_OK;
        delay(p, 1);
    }
    return GIULIA_POWER_TIMEOUT;
}

static int
reset_pulse(struct giulia_pcie_power *p, unsigned i, unsigned hold_us,
    unsigned settle_us)
{
    int r = change(p, i, B(0), B(0));
    if (r)
        return r;
    delay(p, hold_us);
    r = change(p, i, B(0), 0);
    if (!r && settle_us)
        delay(p, settle_us);
    return r;
}

static int
pipe_off(struct giulia_pcie_power *p)
{
    int r = change(p, VOTES1, B(4), 0);
    if (r)
        return r;
    /* PIPE is HALT_SKIP. Park its disabled mux on XO (Linux phy-mux policy). */
    return change(p, PIPE_MUX, 3u, 2u);
}

static int
restore_field(struct giulia_pcie_power *p, unsigned i)
{
    int r;
    if (!(p->dirty & B(i)))
        return GIULIA_POWER_OK;
    r = change(p, i, regs[i].mask, p->saved[i]);
    if (!r)
        p->dirty &= ~B(i);
    return r;
}

/* The caller has stopped all users. Stop at the first failed restoration,
 * retaining supply/NoC ownership and remaining snapshots for a bounded retry.
 */
static int
restore(struct giulia_pcie_power *p)
{
    unsigned i;
    int r;
    if ((p->dirty & (B(VOTES1) | B(PIPE_MUX))) && (r = pipe_off(p)))
        return r;
    /* Hold blocks isolated during clock restoration. BCR snapshots remain
     * dirty until their original values have been restored below.
     */
    for (i = CORE_RESET; i <= PHY_RESET; i++) {
        if (p->dirty & B(i)) {
            if ((r = change(p, i, B(0), B(0))))
                return r;
            delay(p, i == CORE_RESET ? 1000 : 200);
        }
    }
    if (p->dirty & (B(RCH_CMD) | B(RCH_CFG))) {
        r = rcg_apply(p, RCH_CMD, RCH_CFG, p->saved[RCH_CFG],
            p->saved[RCH_CMD]);
        if (r)
            return r;
        p->dirty &= ~(B(RCH_CMD) | B(RCH_CFG));
    }
    if (p->dirty & (B(AUX_CMD) | B(AUX_CFG))) {
        r = rcg_apply(p, AUX_CMD, AUX_CFG, p->saved[AUX_CFG],
            p->saved[AUX_CMD]);
        if (r)
            return r;
        p->dirty &= ~(B(AUX_CMD) | B(AUX_CFG));
    }
    if ((r = restore_field(p, VOTES1)) || (r = restore_field(p, VOTES0)))
        return r;
    delay(p, 10); /* Voted branch disable: other voters may keep clocks on. */
    if ((r = restore_field(p, REF)))
        return r;
    delay(p, 10); /* TCSR ref branch is BRANCH_HALT_DELAY. */
    if ((r = restore_field(p, PIPE_MUX)) ||
        (r = restore_field(p, PHY_RESET)) ||
        (r = restore_field(p, CORE_RESET)) ||
        (r = restore_field(p, PHY_GDSC)) ||
        (r = restore_field(p, CORE_GDSC)))
        return r;
    if (p->dirty & B(COLLAPSE)) {
        r = change(p, COLLAPSE, regs[COLLAPSE].mask, p->saved[COLLAPSE]);
        if (r)
            return r;
        /* VOTABLE OFF does not mean physical OFF; do not wait for CFG OFF. */
        delay(p, 500);
        p->dirty &= ~B(COLLAPSE);
    }
    return GIULIA_POWER_OK;
}

/* An inherited clock vote is not proof of a running PCIe host. The Ace 5
 * handoff has both domains DOWN while this dedicated branch still runs.
 * Only an explicit external clock lease allows reconfiguration; keep its
 * original vote continuously set so stop does not turn off an inherited
 * resource. Status bits are from the same fixed gdsc.c / clk-branch.c above.
 */
static int
rchng_handoff_admit(struct giulia_pcie_power *p)
{
    static const uint32_t offsets[] = {0x6b004, 0x6b008, 0x6c000,
        0x6c004, 0x5214c, 0x6b054};
    uint32_t v[6];
    unsigned i;
    for (i = 0; i < 6; i++) {
        v[i] = p->io.read32(p->io.cookie, GIULIA_POWER_GCC, offsets[i]);
        if (v[i] == 0xffffffffu) return GIULIA_POWER_READBACK;
    }
    if ((v[0] & 7u) != 1u || (v[2] & 7u) != 1u ||
        (v[1] & (B(15) | B(16))) != B(15) ||
        (v[3] & (B(15) | B(16))) != B(15) ||
        (v[4] & 9u) != 9u || (v[5] & B(31)))
        return GIULIA_POWER_BUSY;
    return GIULIA_POWER_OK;
}

static int
start_failed(struct giulia_pcie_power *p, int r, int restore_now)
{
    p->state = GIULIA_POWER_FAULT;
    p->last_error = r;
    p->cleanup_error = restore_now ? restore(p) : 0;
    if (restore_now && !p->cleanup_error)
        p->state = GIULIA_POWER_STOPPED;
    return r;
}

int
giulia_pcie_power_prepare_host(struct giulia_pcie_power *p,
    const struct giulia_pcie_power_io *io, uint32_t prerequisites)
{
    unsigned i;
    int r;
    uint32_t value, aux_cmd, rch_cmd;
    if (!p || !io || !io->read32 || !io->write32 || !io->barrier || !io->delay_us)
        return GIULIA_POWER_ARGUMENT;
    uint32_t route = prerequisites & (GIULIA_POWER_DIRECT_VOTES | GIULIA_POWER_CESTA_COORDINATED);
    if ((prerequisites & GIULIA_POWER_HOST_REQUIRED) != GIULIA_POWER_HOST_REQUIRED ||
        (route != GIULIA_POWER_DIRECT_VOTES && route != GIULIA_POWER_CESTA_COORDINATED) ||
        (route == GIULIA_POWER_DIRECT_VOTES && !(prerequisites & GIULIA_POWER_NOC_READY)))
        return GIULIA_POWER_PREREQUISITE;
    if (p->state != GIULIA_POWER_IDLE && p->state != GIULIA_POWER_STOPPED)
        return GIULIA_POWER_STATE;
    p->io = *io;
    p->dirty = 0;
    p->inherited_rchng = 0;
    p->reset_attempted = 0;
    p->prerequisites = GIULIA_POWER_HOST_REQUIRED | route |
        (prerequisites & GIULIA_POWER_RCHNG_HANDOFF);
    p->last_error = p->cleanup_error = 0;
    for (i = 0; i < GIULIA_POWER_SAVED_REGS; i++) {
        value = read_reg(p, i);
        if (value == 0xffffffffu)
            return GIULIA_POWER_READBACK;
        p->saved[i] = value & regs[i].mask;
    }
    aux_cmd = read_reg(p, AUX_CMD);
    rch_cmd = read_reg(p, RCH_CMD);
    if (aux_cmd == 0xffffffffu || rch_cmd == 0xffffffffu)
        return GIULIA_POWER_READBACK;
    /* Shared NoC and ref votes may be inherited; live PCIe0 leaves may not.
     * No write occurs before this admission check completes.
     */
    if ((p->saved[VOTES1] & 0x3fu) ||
        (p->saved[CORE_GDSC] & 6u) || (p->saved[PHY_GDSC] & 6u) ||
        (p->saved[PIPE_MUX] != 0 && p->saved[PIPE_MUX] != 2) ||
        (aux_cmd & ROOT_UPDATE) || (rch_cmd & ROOT_UPDATE))
        return GIULIA_POWER_BUSY;
    if (p->saved[VOTES0] & B(22)) {
        if (!(prerequisites & GIULIA_POWER_RCHNG_HANDOFF))
            return GIULIA_POWER_BUSY;
        if (rch_cmd & ROOT_OFF) return GIULIA_POWER_BUSY;
        if ((r = rchng_handoff_admit(p))) return r;
        p->inherited_rchng = 1;
    }
    p->state = GIULIA_POWER_FAULT; /* Any attempted write now needs cleanup. */
    if ((r = gdsc_on(p, CORE_GDSC, B(0))) ||
        (r = gdsc_on(p, PHY_GDSC, B(3))) ||
        /* F(19.2M, XO, 1, 0, 0): HID=2*1-1; source selector 0.
         * F(100M, GPLL0_EVEN, 3, 0, 0): HID=5; source selector 6.
         * Both have N=0: integer mode, no change to AUX M/N/D registers.
         */
        (r = rcg_apply(p, AUX_CMD, AUX_CFG, 1u, p->saved[AUX_CMD])) ||
        (r = rcg_apply(p, RCH_CMD, RCH_CFG, 0x605u, p->saved[RCH_CMD])) ||
        (r = pipe_off(p)))
        goto fail;
    for (i = 0; i < 8; i++)
        if ((r = branch_on(p, &branches[i])))
            goto fail;
    p->state = GIULIA_POWER_HOST_PREPARED;
    return GIULIA_POWER_OK;
fail:
    /* No external managed profile has been submitted by this API. */
    return start_failed(p, r, 1);
}

int
giulia_pcie_power_finish_reset(struct giulia_pcie_power *p, uint32_t prerequisites)
{
    int r;
    if (!p) return GIULIA_POWER_ARGUMENT;
    if (p->state != GIULIA_POWER_HOST_PREPARED) return GIULIA_POWER_STATE;
    uint32_t route = prerequisites & (GIULIA_POWER_DIRECT_VOTES | GIULIA_POWER_CESTA_COORDINATED);
    uint32_t required = p->prerequisites | GIULIA_POWER_NOC_READY;
    if ((prerequisites & required) != required ||
        route != (p->prerequisites & (GIULIA_POWER_DIRECT_VOTES | GIULIA_POWER_CESTA_COORDINATED)))
        return GIULIA_POWER_PREREQUISITE;
    p->reset_attempted = 1;
    p->state = GIULIA_POWER_FAULT;
    if ((r = reset_pulse(p, CORE_RESET, 1000, 1000)) ||
        (r = reset_pulse(p, PHY_RESET, 200, 0)) ||
        (r = change(p, REF, B(0), B(0))))
        goto fail;
    delay(p, 10);
    if ((r = branch_on(p, &branches[8])))
        goto fail;
    p->state = GIULIA_POWER_READY;
    return GIULIA_POWER_OK;
fail:
    /* A managed profile may need host clocks to complete withdrawal. Never
     * tear its foundation away just because a reset/ref write failed. */
    return start_failed(p, r, !(p->prerequisites & GIULIA_POWER_CESTA_COORDINATED));
}

int
giulia_pcie_power_start(struct giulia_pcie_power *p,
    const struct giulia_pcie_power_io *io, uint32_t prerequisites)
{
    int r;
    if (!p || !io || !io->read32 || !io->write32 || !io->barrier || !io->delay_us)
        return GIULIA_POWER_ARGUMENT;
    if ((prerequisites & GIULIA_POWER_REQUIRED) != GIULIA_POWER_REQUIRED ||
        (prerequisites & GIULIA_POWER_CESTA_COORDINATED)) return GIULIA_POWER_PREREQUISITE;
    r = giulia_pcie_power_prepare_host(p, io, prerequisites);
    return r ? r : giulia_pcie_power_finish_reset(p, prerequisites);
}

int
giulia_pcie_power_pipe_enable(struct giulia_pcie_power *p)
{
    int r;
    if (!p)
        return GIULIA_POWER_ARGUMENT;
    if (p->state != GIULIA_POWER_READY)
        return GIULIA_POWER_STATE;
    if ((r = change(p, PIPE_MUX, 3u, 0)) ||
        (r = change(p, VOTES1, B(4), B(4)))) {
        p->last_error = r;
        p->state = GIULIA_POWER_FAULT;
        return r;
    }
    p->state = GIULIA_POWER_PIPE_ON;
    return GIULIA_POWER_OK;
}

int
giulia_pcie_power_pipe_disable(struct giulia_pcie_power *p)
{
    int r;
    if (!p)
        return GIULIA_POWER_ARGUMENT;
    if (p->state != GIULIA_POWER_READY && p->state != GIULIA_POWER_PIPE_ON &&
        p->state != GIULIA_POWER_FAULT)
        return GIULIA_POWER_STATE;
    r = pipe_off(p);
    if (r) {
        p->last_error = r;
        p->state = GIULIA_POWER_FAULT;
    } else if (p->state == GIULIA_POWER_PIPE_ON) {
        p->state = GIULIA_POWER_READY;
    }
    return r;
}

int
giulia_pcie_power_stop(struct giulia_pcie_power *p, uint32_t conditions)
{
    int r;
    if (!p)
        return GIULIA_POWER_ARGUMENT;
    if (p->state == GIULIA_POWER_IDLE || p->state == GIULIA_POWER_STOPPED)
        return GIULIA_POWER_OK;
    if ((conditions & GIULIA_POWER_STOP_REQUIRED) != GIULIA_POWER_STOP_REQUIRED)
        return GIULIA_POWER_PREREQUISITE;
    if ((p->prerequisites & GIULIA_POWER_CESTA_COORDINATED) &&
        !(conditions & GIULIA_POWER_STOP_CESTA_RESOURCES_QUIET))
        return GIULIA_POWER_PREREQUISITE;
    if (p->state != GIULIA_POWER_READY && p->state != GIULIA_POWER_PIPE_ON &&
        p->state != GIULIA_POWER_FAULT && p->state != GIULIA_POWER_HOST_PREPARED)
        return GIULIA_POWER_STATE;
    r = restore(p);
    p->cleanup_error = r;
    p->state = r ? GIULIA_POWER_FAULT : GIULIA_POWER_STOPPED;
    return r;
}
