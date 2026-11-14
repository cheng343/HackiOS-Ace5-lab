#ifdef NDEBUG
#error "Loader tests require assertions: use -UNDEBUG"
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "watchdog-budget.inc"

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
    return 0;
}
