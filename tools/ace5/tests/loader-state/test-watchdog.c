#ifdef NDEBUG
#error "Loader tests require assertions: use -UNDEBUG"
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "watchdog-budget.inc"
#include "giulia_gunyah_watchdog.h"

#define VIRT_WDT_STATUS GIULIA_WDT_STATUS
#define print(...) ((void)0)

static const uint64_t trial_order[] = {
    GIULIA_WDT_SET_TIME, GIULIA_WDT_SET_TIME, GIULIA_WDT_CONTROL, GIULIA_WDT_PET, GIULIA_WDT_STATUS
};
static unsigned trial_calls;
static unsigned fail_call;
static uint64_t status_bits;
#define DIRTY_HIGH UINT64_C(0xa5a5111100000000)

static uint64_t
smc_call(uint64_t id, uint64_t a1, uint64_t a2, uint64_t *r1, uint64_t *r2)
{
    assert(trial_calls < sizeof(trial_order) / sizeof(trial_order[0]));
    assert(id == trial_order[trial_calls++]);
    if (id == GIULIA_WDT_SET_TIME) {
        assert(!r1 && !r2);
        if (trial_calls == 1) {
            assert(a1 == 11000 && a2 == 0xffff);
        } else {
            assert(a1 == 0xffff && a2 == 14000);
        }
    } else if (id == GIULIA_WDT_CONTROL) {
        assert(a1 == 3 && a2 == 0 && !r1 && !r2);
    } else if (id == GIULIA_WDT_PET) {
        assert(!a1 && !a2 && !r1 && !r2);
    } else {
        assert(!a1 && !a2 && r1);
        *r1 = DIRTY_HIGH | status_bits;
        if (r2) *r2 = DIRTY_HIGH | 1;
    }
    return DIRTY_HIGH | (trial_calls == fail_call ? UINT32_MAX : 0);
}

#include "watchdog-smc32.inc"
#include "watchdog-trial.inc"

int main(void)
{
    /* No-timer sessions must not lose the observer's watchdog feed at 60 s. */
    const uint64_t persistent[] = {0, 59, 60, 65, 120, 3600, 86400, UINT64_MAX};
    for (unsigned i = 0; i < sizeof(persistent) / sizeof(persistent[0]); i++)
        assert(observer_watchdog_within_budget(persistent[i], 0));
    /* Timed diagnostics retain exactly their existing 60-second grace period. */
    const uint64_t budgets[] = {1, 15, 60, 180, 3600};
    for (unsigned i = 0; i < sizeof(budgets) / sizeof(budgets[0]); i++) {
        uint64_t b = budgets[i];
        assert(observer_watchdog_within_budget(0, b));
        assert(observer_watchdog_within_budget(b - 1, b));
        assert(observer_watchdog_within_budget(b, b));
        assert(observer_watchdog_within_budget(b + 59, b));
        assert(!observer_watchdog_within_budget(b + 60, b));
        assert(!observer_watchdog_within_budget(b + 61, b));
    }
    assert(observer_watchdog_within_budget(UINT64_MAX, UINT64_MAX));
    assert(observer_watchdog_within_budget(UINT64_MAX, UINT64_MAX - 59));
    assert(!observer_watchdog_within_budget(UINT64_MAX, UINT64_MAX - 60));
    puts("PASS: actual observer watchdog budget handles unlimited, finite and overflow boundaries");
    assert(!watchdog_trial_reply && GIULIA_WDT_FALLBACK_SECONDS * 1000 > GIULIA_WDT_BITE_MS);
    uint64_t detail;
    const char *errors[] = {"SET_BARK", "SET_BITE", "ENABLE", "PET", "STATUS"};
    status_bits = GIULIA_WDT_ENABLED;
    for (fail_call = 1; fail_call <= 5; fail_call++) {
        trial_calls = 0;
        assert(strcmp(giulia_wdt_arm_trial(&detail), errors[fail_call - 1]) == 0);
        assert(detail == UINT32_MAX && trial_calls == fail_call);
    }
    trial_calls = 0;
    assert(giulia_wdt_arm_trial(&detail) == NULL && trial_calls == 5);
    trial_calls = 0;
    status_bits = 0;
    assert(strcmp(giulia_wdt_arm_trial(&detail), "STATUS_CTL") == 0 && detail == 0);
    trial_calls = 0;
    status_bits = GIULIA_WDT_ENABLED | GIULIA_WDT_EXPIRED;
    assert(strcmp(giulia_wdt_arm_trial(&detail), "STATUS_CTL") == 0 && detail == status_bits);
    puts("PASS: Giulia watchdog trial checks each SMC and verifies enabled, unexpired status");
    return 0;
}
