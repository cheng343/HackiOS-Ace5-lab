/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef GIULIA_GUNYAH_WATCHDOG_H
#define GIULIA_GUNYAH_WATCHDOG_H

#include <stdint.h>

#define GIULIA_WDT_CONTROL       0x86000005u
#define GIULIA_WDT_STATUS        0x86000006u
#define GIULIA_WDT_PET           0x86000007u
#define GIULIA_WDT_SET_TIME      0x86000008u
#define GIULIA_WDT_ENABLED       1u
#define GIULIA_WDT_EXPIRED       (1u << 31)
#define GIULIA_WDT_BARK_MS       11000u
#define GIULIA_WDT_BITE_MS       14000u
#define GIULIA_WDT_NO_CHANGE     0xffffu

typedef uint64_t (*giulia_wdt_call_t)(uint64_t, uint64_t, uint64_t,
    uint64_t *, uint64_t *);

/* The vendor driver programs bark and bite separately. The caller must own
 * the watchdog and have read back an initially disabled state. */
static inline const char *
giulia_wdt_enable_checked(giulia_wdt_call_t call, uint64_t *detail, int *owned)
{
    uint64_t ctl = 0;
    *detail = call(GIULIA_WDT_CONTROL, 3, 0, 0, 0);
    if (*detail) return "ENABLE";
    if (owned) *owned = 1; /* only a successful enable grants ownership */
    *detail = call(GIULIA_WDT_PET, 0, 0, 0, 0);
    if (*detail) return "PET";
    *detail = call(GIULIA_WDT_STATUS, 0, 0, &ctl, 0);
    if (*detail) return "STATUS";
    *detail = ctl;
    if ((ctl & (GIULIA_WDT_ENABLED | GIULIA_WDT_EXPIRED)) != GIULIA_WDT_ENABLED)
        return "STATUS_CTL";
    return 0;
}

/* Preserve the established short configuration used by XNU and other loader
 * modes. The pre-handoff path sets a longer pair while disabled instead. */
static inline const char *
giulia_wdt_configure(giulia_wdt_call_t call, uint64_t *detail)
{
    *detail = call(GIULIA_WDT_SET_TIME, GIULIA_WDT_BARK_MS, GIULIA_WDT_NO_CHANGE, 0, 0);
    if (*detail) return "SET_BARK";
    *detail = call(GIULIA_WDT_SET_TIME, GIULIA_WDT_NO_CHANGE, GIULIA_WDT_BITE_MS, 0, 0);
    if (*detail) return "SET_BITE";
    return giulia_wdt_enable_checked(call, detail, 0);
}

#endif
