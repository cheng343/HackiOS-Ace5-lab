/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_GEM_CLOCK_H
#define GIULIA_GEM_CLOCK_H
#include <stdint.h>

/* The GEM QoS provider consumes GCC_CPUSS_UBWCP_CLK_SRC, not its branch.
 * This narrow lease supports only an inherited, committed XO/div1 config.
 * No PLL, CFG, UPDATE, branch, RPMh or firmware writes are made here. */
#define GIULIA_GEM_GCC_PHYS 0x100000u
#define GIULIA_GEM_GCC_SIZE 0x1f4200u
#define GIULIA_GEM_XO_HZ 19200000u
/* RPMh corner encoding, NOT microvolts. Vendor prepare() selects LOWER
 * even at XO because the first nonzero rate_max entry is 300 MHz. */
#define GIULIA_GEM_CX_AO_MIN_LEVEL 64u
enum giulia_gem_state {
    GIULIA_GEM_IDLE, GIULIA_GEM_PREPARING, GIULIA_GEM_ACTIVE,
    GIULIA_GEM_FAULT, GIULIA_GEM_STOPPED
};
enum giulia_gem_result {
    GIULIA_GEM_ARGUMENT=-1, GIULIA_GEM_STATE=-2, GIULIA_GEM_LAYOUT=-3,
    GIULIA_GEM_LEASE=-4, GIULIA_GEM_IO=-5, GIULIA_GEM_READBACK=-6,
    GIULIA_GEM_BUSY=-7, GIULIA_GEM_CONFIG=-8, GIULIA_GEM_TIMEOUT=-9
};
struct giulia_gem_clock_io {
    void *cookie;
    int (*read32)(void *, uint32_t offset, uint32_t *);
    /* A failed write may have committed; the object retains its baseline. */
    int (*write32)(void *, uint32_t offset, uint32_t);
    void (*barrier)(void *);
    void (*delay_us)(void *, unsigned);
    /* Real platform owner must retain XO parent and completed CX_AO >=64
     * corner vote, GCC access, exclusive RCG writers, firmware coordination,
     * quiet shared clients and no low-power transition THROUGH stop.
     * Recheck before every MMIO. Android dumps or a success stub are not
     * ownership. NoC proxy votes are additional QoS prerequisites. */
    int (*validate_lease)(void *);
    uint32_t gcc_phys, gcc_size;
};
struct giulia_gem_clock {
    struct giulia_gem_clock_io io;
    uint32_t state, owned, dirty, saved_force, saved_cfg, valid;
    uint32_t failed_offset, observed;
    int error, cleanup_error, callback_error;
};
/* Zero once; serialize calls; keep the object at a stable address until
 * STOPPED. Hold ROOT_EN for the entire configuration-access interval and
 * verify ROOT_OFF clears within 500 us. ACTIVE is clock status only, not
 * physical NoC/PCIe readiness. No automatic XNU boot binding. */
int giulia_gem_clock_start(struct giulia_gem_clock *, const struct giulia_gem_clock_io *);
/* A retained FAULT may return to ACTIVE only after the real owner repairs
 * dependencies AND this read-only check revalidates config/force/ON status. */
int giulia_gem_clock_check(struct giulia_gem_clock *);
/* Restore only our ROOT_EN change after QoS users finish, never an inherited
 * force vote. CFG drift/pending UPDATE retains FAULT and dependencies for
 * owner repair; retry does not overwrite foreign config or resend UPDATE. */
int giulia_gem_clock_stop(struct giulia_gem_clock *);
#endif
