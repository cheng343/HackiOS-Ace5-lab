/* SPDX-License-Identifier: BSD-2-Clause */
/* Original implementation; register facts and prerequisites checked against:
 * OnePlusOSS/android_kernel_oneplus_sm8650@66305c56af1fcd7299c906c3257e585be93d93ed
 * drivers/interconnect/qcom/{pineapple.c,qnoc-qos.c,icc-rpmh.c}:
 * xm_pcie3_0/qnm_pcie, set_qos and enable_qos_deps.
 * OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650@
 * 453d8875f815bf9dfad9035982861226a4823a6c: pineapple.dtsi NoC resources.
 * Linux v6.12 drivers/clk/qcom/gcc-sm8650.c: aggregate/config PCIe ANOC
 * clocks, 0x52000 bits12/20, both BRANCH_HALT_SKIP; do not poll/fake HWCG.
 * No Linux framework source copied; rollback and ownership are local design.
 */
#include "giulia_pcie_qos.h"
#define BIT(n) (1u << (n))
#define VOTES (BIT(12) | BIT(20))
#define FIELDS (BIT(24) | 0x70u | BIT(3))
/* Save slots are clock, downstream GEM, upstream PCIe. */
static const unsigned blocks[3] = {GIULIA_QOS_GCC, GIULIA_QOS_GEM, GIULIA_QOS_PCIE};
static const uint32_t offsets[3] = {0x52000u, 0xb7008u, 0xb008u};
static const uint32_t masks[3] = {VOTES, FIELDS, FIELDS};
static const uint32_t values[3] = {VOTES, 0x28u, 0x30u};
static uintptr_t qos_owner;

static int owned(const struct giulia_pcie_qos *p)
{ return p->owned && __atomic_load_n(&qos_owner,__ATOMIC_ACQUIRE) == (uintptr_t)p; }
static int fault(struct giulia_pcie_qos *p, int rc)
{ p->state = GIULIA_QOS_FAULT; return rc; }
static int lease(struct giulia_pcie_qos *p, unsigned i)
{
    p->failed_block = blocks[i]; p->failed_offset = offsets[i];
    p->observed_valid = 0;
    p->callback_error = p->io.validate_lease(p->io.cookie);
    return p->callback_error ? GIULIA_QOS_LEASE : 0;
}
static int read_reg(struct giulia_pcie_qos *p, unsigned i, uint32_t *value)
{
    int rc;
    uint32_t clocks;
    /* Do not issue even a restoration read into NoC after our branch votes
     * disappear. Retain FAULT until the owning platform repairs access. */
    if (i) {
        if ((rc = read_reg(p,0,&clocks))) return rc;
        if ((clocks & VOTES) != VOTES) {
            p->expected = VOTES;
            return GIULIA_QOS_READBACK;
        }
    }
    rc = lease(p,i);
    if (rc) return rc;
    p->callback_error = p->io.read32(p->io.cookie,blocks[i],offsets[i],value);
    if (p->callback_error) return GIULIA_QOS_IO;
    p->observed = *value; p->observed_valid = 1;
    return *value == UINT32_MAX ? GIULIA_QOS_READBACK : 0;
}
static int change(struct giulia_pcie_qos *p, unsigned i, uint32_t mask,
    uint32_t value, int restoring)
{
    uint32_t old, want, got;
    int rc = read_reg(p,i,&old);
    if (rc) return rc;
    want = (old & ~mask) | (value & mask);
    p->expected = want;
    if (old == want) return 0;
    if ((rc = lease(p,i))) return rc;
    /* This baseline was read before any write; a failed callback may have
     * committed. Mark BEFORE calling it. Stop must never replace baseline. */
    if (!restoring) p->dirty |= BIT(i);
    p->callback_error = p->io.write32(p->io.cookie,blocks[i],offsets[i],want);
    p->io.barrier(p->io.cookie);
    if (p->callback_error) return GIULIA_QOS_IO;
    if ((rc = read_reg(p,i,&got))) return rc;
    return ((got ^ want) & mask) ? GIULIA_QOS_READBACK : 0;
}
static int release(struct giulia_pcie_qos *p)
{
    uintptr_t expected = (uintptr_t)p;
    if (!__atomic_compare_exchange_n(&qos_owner,&expected,0,0,__ATOMIC_RELEASE,__ATOMIC_RELAXED))
        return GIULIA_QOS_STATE;
    p->owned = 0; p->state = GIULIA_QOS_STOPPED;
    return 0;
}
static int verify(struct giulia_pcie_qos *p)
{
    uint32_t v;
    int rc;
    for (unsigned i=0;i<3;i++) {
        p->expected = values[i];
        if ((rc = read_reg(p,i,&v))) return rc;
        if ((v & masks[i]) != values[i]) return GIULIA_QOS_READBACK;
    }
    return 0;
}
int giulia_pcie_qos_check(struct giulia_pcie_qos *p)
{
    int rc;
    if (!p) return GIULIA_QOS_ARGUMENT;
    if (!owned(p) || p->state != GIULIA_QOS_ACTIVE) return GIULIA_QOS_STATE;
    rc = verify(p);
    if (rc) { p->error = rc; return fault(p,rc); }
    return 0;
}
int giulia_pcie_qos_stop(struct giulia_pcie_qos *p)
{
    int rc;
    if (!p) return GIULIA_QOS_ARGUMENT;
    if (p->state == GIULIA_QOS_IDLE || p->state == GIULIA_QOS_STOPPED) return 0;
    if (!owned(p)) return GIULIA_QOS_STATE;
    p->cleanup_error = 0;
    /* The clock is still held until both NoC snapshots are restored. Even
     * dirty==0 retains the external lifetime until its owner validates. */
    if ((rc = lease(p,0))) goto failed;
    for (unsigned n=3;n>0;n--) {
        unsigned i = n-1;
        if (!(p->dirty & BIT(i))) continue;
        if (!(p->valid & BIT(i))) { rc = GIULIA_QOS_STATE; goto failed; }
        if ((rc = change(p,i,masks[i],p->saved[i],1))) goto failed;
        p->dirty &= ~BIT(i);
    }
    rc = release(p);
    if (!rc) return 0;
failed:
    p->cleanup_error = rc;
    return fault(p,rc);
}
static int start_failed(struct giulia_pcie_qos *p, int rc)
{
    /* Keep initial diagnostics even if the best-effort rollback succeeds. */
    uint32_t block=p->failed_block, off=p->failed_offset;
    uint32_t exp=p->expected, obs=p->observed, valid=p->observed_valid;
    int callback=p->callback_error;
    p->error=rc; p->state=GIULIA_QOS_FAULT;
    (void)giulia_pcie_qos_stop(p);
    p->failed_block=block; p->failed_offset=off; p->expected=exp;
    p->observed=obs; p->observed_valid=valid; p->callback_error=callback;
    return rc;
}
int giulia_pcie_qos_start(struct giulia_pcie_qos *p,
    const struct giulia_pcie_qos_io *io, uint32_t conditions)
{
    const uint32_t phys[3]={GIULIA_QOS_GCC_PHYS,GIULIA_QOS_PCIE_PHYS,GIULIA_QOS_GEM_PHYS};
    const uint32_t size[3]={GIULIA_QOS_GCC_SIZE,GIULIA_QOS_PCIE_SIZE,GIULIA_QOS_GEM_SIZE};
    const uint32_t fields[3]={BIT(24),0x70u,BIT(3)};
    uintptr_t expected=0;
    int rc;
    if (!p || !io || !io->read32 || !io->write32 || !io->barrier || !io->validate_lease)
        return GIULIA_QOS_ARGUMENT;
    if (p->state != GIULIA_QOS_IDLE && p->state != GIULIA_QOS_STOPPED) return GIULIA_QOS_STATE;
    if ((conditions & GIULIA_QOS_REQUIRED) != GIULIA_QOS_REQUIRED) return GIULIA_QOS_PREREQUISITE;
    for (unsigned i=0;i<3;i++)
        if (io->phys[i] != phys[i] || io->size[i] != size[i]) return GIULIA_QOS_LAYOUT;
    if (io->validate_lease(io->cookie)) return GIULIA_QOS_LEASE;
    struct giulia_pcie_qos_io copy=*io;
    if (!__atomic_compare_exchange_n(&qos_owner,&expected,(uintptr_t)p,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))
        return GIULIA_QOS_STATE;
    *p=(struct giulia_pcie_qos){0}; p->io=copy; p->owned=1; p->state=GIULIA_QOS_PREPARING;
    if ((rc=read_reg(p,0,&p->saved[0]))) return start_failed(p,rc);
    p->valid |= BIT(0);
    if ((rc=change(p,0,BIT(12),BIT(12),0)) ||
        (rc=change(p,0,BIT(20),BIT(20),0))) return start_failed(p,rc);
    /* Both branch types are HALT_SKIP. A vote readback is not a physical
     * NoC-ready proof; external completed proxies/access conditions remain. */
    for (unsigned i=1;i<3;i++) {
        if ((rc=read_reg(p,i,&p->saved[i]))) return start_failed(p,rc);
        p->valid |= BIT(i);
    }
    for (unsigned i=1;i<3;i++)
        for (unsigned f=0;f<3;f++)
            if ((rc=change(p,i,fields[f],values[i],0))) return start_failed(p,rc);
    if ((rc=verify(p))) return start_failed(p,rc);
    p->state=GIULIA_QOS_ACTIVE;
    return 0;
}
