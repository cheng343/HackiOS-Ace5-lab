#ifdef NDEBUG
#error "Loader tests require assertions: use -UNDEBUG"
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "watchdog-budget.inc"

#define VIRT_WDT_CONTROL 5
#define VIRT_WDT_STATUS 6
#define VIRT_WDT_PET 7
#define VIRT_WDT_SET_TIME 8
#define print(...) ((void)0)

static const uint64_t trial_order[] = {
    VIRT_WDT_SET_TIME, VIRT_WDT_CONTROL, VIRT_WDT_PET, VIRT_WDT_STATUS
};
static unsigned trial_calls;
static unsigned fail_call;
static uint64_t status_bits;

static uint64_t
smc_call(uint64_t id, uint64_t a1, uint64_t a2, uint64_t *r1, uint64_t *r2)
{
    assert(trial_calls < sizeof(trial_order) / sizeof(trial_order[0]));
    assert(id == trial_order[trial_calls++]);
    if (id == VIRT_WDT_SET_TIME) {
        assert(a1 == 8000 && a2 == 8000 && !r1 && !r2);
    } else if (id == VIRT_WDT_CONTROL) {
        assert(a1 == 3 && a2 == 0 && !r1 && !r2);
    } else if (id == VIRT_WDT_PET) {
        assert(!a1 && !a2 && !r1 && !r2);
    } else {
        assert(!a1 && !a2 && r1 && r2);
        *r1 = status_bits;
        *r2 = 1;
    }
    return trial_calls == fail_call;
}

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
    assert(!watchdog_trial_reply && GIULIA_WDT_FALLBACK_SECONDS * 1000 > GIULIA_WDT_TRIAL_MS);
    status_bits = WDT_CTL_ENABLED;
    for (fail_call = 1; fail_call <= 4; fail_call++) {
        trial_calls = 0;
        assert(!giulia_wdt_arm_trial() && trial_calls == fail_call);
    }
    trial_calls = 0;
    assert(giulia_wdt_arm_trial() && trial_calls == 4);
    trial_calls = 0;
    status_bits = 0;
    assert(!giulia_wdt_arm_trial());
    trial_calls = 0;
    status_bits = WDT_CTL_ENABLED | WDT_CTL_EXPIRED;
    assert(!giulia_wdt_arm_trial());
    puts("PASS: Giulia watchdog trial checks each SMC and verifies enabled, unexpired status");
    return 0;
}
