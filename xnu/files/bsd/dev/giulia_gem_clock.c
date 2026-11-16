/* SPDX-License-Identifier: BSD-2-Clause */
/* Original implementation based on register/protocol facts in OnePlusOSS
 * android_kernel_oneplus_sm8650@66305c56af1fcd7299c906c3257e585be93d93ed:
 * drivers/clk/qcom/{gcc-pineapple.c,clk-rcg2.c,clk-regmap.c}.
 * No Linux framework code copied. Unlike clk_rcg2_enable(), this narrow
 * lease preserves an already committed XO config and holds force-enable
 * until its users finish. Unsupported inherited rates are left untouched. */
#include "giulia_gem_clock.h"
#define BIT(n) (1u<<(n))
#define CMD 0x63004u
#define CFG 0x63008u
#define FORCE BIT(1)
#define OFF BIT(31)
#define PENDING (BIT(0)|BIT(4)|BIT(5)|BIT(6)|BIT(7))
#define CFG_FIELDS (0x1fu|0x700u|0x3000u|BIT(20))
#define XO_CFG (BIT(20)|1u)
static uintptr_t gem_owner;

static int owner(const struct giulia_gem_clock *p)
{ return p->owned && __atomic_load_n(&gem_owner,__ATOMIC_ACQUIRE)==(uintptr_t)p; }
static int lease(struct giulia_gem_clock *p,uint32_t off)
{
    p->failed_offset=off;
    p->callback_error=p->io.validate_lease(p->io.cookie);
    return p->callback_error ? GIULIA_GEM_LEASE : 0;
}
static int read_reg(struct giulia_gem_clock *p,uint32_t off,uint32_t *v)
{
    int rc=lease(p,off);
    if (rc) return rc;
    p->callback_error=p->io.read32(p->io.cookie,off,v);
    if (p->callback_error) return GIULIA_GEM_IO;
    p->observed=*v;
    return *v==UINT32_MAX ? GIULIA_GEM_READBACK : 0;
}
static int stable(struct giulia_gem_clock *p,uint32_t *cmd)
{
    uint32_t cfg;
    int rc=read_reg(p,CMD,cmd);
    if (rc) return rc;
    if (*cmd&PENDING) return GIULIA_GEM_BUSY;
    if ((rc=read_reg(p,CFG,&cfg))) return rc;
    if ((cfg&CFG_FIELDS)!=XO_CFG) return GIULIA_GEM_CONFIG;
    if (p->valid && cfg!=p->saved_cfg) return GIULIA_GEM_CONFIG;
    return 0;
}
static int force(struct giulia_gem_clock *p,uint32_t value,int restoring)
{
    uint32_t cmd,got;
    int rc=stable(p,&cmd);
    if (rc) return rc;
    if ((cmd&FORCE)==value) return 0;
    if ((rc=lease(p,CMD))) return rc;
    if (!restoring) p->dirty=1;
    p->callback_error=p->io.write32(p->io.cookie,CMD,(cmd&~FORCE)|value);
    p->io.barrier(p->io.cookie);
    if (p->callback_error) return GIULIA_GEM_IO;
    if ((rc=read_reg(p,CMD,&got))) return rc;
    if (got&PENDING) return GIULIA_GEM_BUSY;
    return (got&FORCE)==value ? 0 : GIULIA_GEM_READBACK;
}
int giulia_gem_clock_stop(struct giulia_gem_clock *p)
{
    uintptr_t expected;
    int rc;
    if (!p) return GIULIA_GEM_ARGUMENT;
    if (p->state==GIULIA_GEM_IDLE || p->state==GIULIA_GEM_STOPPED) return 0;
    if (!owner(p)) return GIULIA_GEM_STATE;
    p->cleanup_error=0;
    if ((rc=lease(p,CMD))) goto failed;
    if (p->dirty) {
        if (!p->valid) { rc=GIULIA_GEM_STATE; goto failed; }
        if ((rc=force(p,p->saved_force,1))) goto failed;
        p->dirty=0;
    }
    expected=(uintptr_t)p;
    if (!__atomic_compare_exchange_n(&gem_owner,&expected,0,0,__ATOMIC_RELEASE,__ATOMIC_RELAXED)) {
        rc=GIULIA_GEM_STATE; goto failed;
    }
    p->owned=0; p->state=GIULIA_GEM_STOPPED;
    return 0;
failed:
    p->cleanup_error=rc; p->state=GIULIA_GEM_FAULT;
    return rc;
}
static int start_failed(struct giulia_gem_clock *p,int rc)
{
    uint32_t off=p->failed_offset,observed=p->observed;
    int callback=p->callback_error;
    p->error=rc; p->state=GIULIA_GEM_FAULT;
    (void)giulia_gem_clock_stop(p);
    p->failed_offset=off; p->observed=observed; p->callback_error=callback;
    return rc;
}
int giulia_gem_clock_check(struct giulia_gem_clock *p)
{
    uint32_t cmd;
    int rc;
    if (!p) return GIULIA_GEM_ARGUMENT;
    if (!owner(p) || !p->valid ||
        (p->state!=GIULIA_GEM_ACTIVE && p->state!=GIULIA_GEM_FAULT)) return GIULIA_GEM_STATE;
    rc=stable(p,&cmd);
    if (!rc && (cmd&(FORCE|OFF))!=FORCE) rc=GIULIA_GEM_READBACK;
    if (rc) { p->error=rc; p->state=GIULIA_GEM_FAULT; }
    else p->state=GIULIA_GEM_ACTIVE;
    return rc;
}
int giulia_gem_clock_start(struct giulia_gem_clock *p,const struct giulia_gem_clock_io *io)
{
    uintptr_t expected=0;
    uint32_t cmd;
    int rc;
    if (!p || !io || !io->read32 || !io->write32 || !io->barrier ||
        !io->delay_us || !io->validate_lease) return GIULIA_GEM_ARGUMENT;
    if (p->state!=GIULIA_GEM_IDLE && p->state!=GIULIA_GEM_STOPPED) return GIULIA_GEM_STATE;
    if (io->gcc_phys!=GIULIA_GEM_GCC_PHYS || io->gcc_size!=GIULIA_GEM_GCC_SIZE)
        return GIULIA_GEM_LAYOUT;
    if (io->validate_lease(io->cookie)) return GIULIA_GEM_LEASE;
    struct giulia_gem_clock_io copy=*io;
    if (!__atomic_compare_exchange_n(&gem_owner,&expected,(uintptr_t)p,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))
        return GIULIA_GEM_STATE;
    *p=(struct giulia_gem_clock){0}; p->io=copy; p->owned=1; p->state=GIULIA_GEM_PREPARING;
    if ((rc=stable(p,&cmd))) return start_failed(p,rc);
    p->saved_force=cmd&FORCE;
    if ((rc=read_reg(p,CFG,&p->saved_cfg))) return start_failed(p,rc);
    p->valid=1;
    if ((rc=force(p,FORCE,0))) return start_failed(p,rc);
    for (unsigned us=0;us<500;us++) {
        if ((rc=read_reg(p,CMD,&cmd))) return start_failed(p,rc);
        if (cmd&PENDING) return start_failed(p,GIULIA_GEM_BUSY);
        if (!(cmd&FORCE)) return start_failed(p,GIULIA_GEM_READBACK);
        if (!(cmd&OFF)) {
            p->state=GIULIA_GEM_ACTIVE;
            if ((rc=giulia_gem_clock_check(p))) return start_failed(p,rc);
            return 0;
        }
        p->io.delay_us(p->io.cookie,1);
    }
    return start_failed(p,GIULIA_GEM_TIMEOUT);
}
