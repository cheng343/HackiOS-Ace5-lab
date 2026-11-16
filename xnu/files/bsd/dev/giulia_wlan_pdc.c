/* SPDX-License-Identifier: BSD-2-Clause */
/* Original ordered AOP table lifecycle. Protocol strings/order from:
 * OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650@
 * 453d8875f815bf9dfad9035982861226a4823a6c:
 * vendor/qcom/proprietary/wlan-devicetree/pineapple-kiwi-cnss.dtsi,
 * vendor/qcom/opensource/wlan/platform/cnss2/power.c:cnss_aop_pdc_reconfig.
 * This code does not claim that a QMP length ACK is a PMIC/table readback.
 */
#include "giulia_wlan_pdc.h"
static const char *const prefix[10] = {
    "{class: wlan_pdc, ss: rf, res: s3c.v, upval: ",
    "{class: wlan_pdc, ss: rf, res: s3c.v, dwnval: ",
    "{class: wlan_pdc, ss: rf, res: s1c.v, upval: ",
    "{class: wlan_pdc, ss: rf, res: s1c.v, dwnval: ",
    "{class: wlan_pdc, ss: rf, res: s6c.v, upval: ",
    "{class: wlan_pdc, ss: rf, res: s6c.v, dwnval: ",
    "{class: wlan_pdc, ss: bb, res: s2c.v, upval: ",
    "{class: wlan_pdc, ss: bb, res: s2c.v, dwnval: ",
    "{class: wlan_pdc, ss: bb, res: s4i.v, upval: ",
    "{class: wlan_pdc, ss: bb, res: s4i.v, dwnval: "
};
static const uint32_t target[10] = {916,612,1316,944,1864,1820,976,512,940,420};
static struct giulia_wlan_pdc *table_owner;
static unsigned message(unsigned i, uint32_t value, char text[64])
{
    unsigned n = 0, digits = 0;
    char reverse[10];
    for (unsigned j = 0; j < 64; j++) text[j] = 0;
    while (prefix[i][n]) { text[n] = prefix[i][n]; n++; }
    do { reverse[digits++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (digits) text[n++] = reverse[--digits];
    text[n++] = '}';
    return n + 1; /* Include terminating NUL in the DT-property comparison. */
}
static int matches(const uint8_t *dt, uint32_t bytes)
{
    char text[64];
    uint32_t at = 0;
    if (!dt) return 0;
    for (unsigned i = 0; i < 10; i++) {
        unsigned n = message(i, target[i], text);
        if (n > bytes - at) return 0;
        for (unsigned j = 0; j < n; j++) if (dt[at+j] != (uint8_t)text[j]) return 0;
        at += n;
    }
    return at == bytes;
}
static int owns(const struct giulia_wlan_pdc *p)
{ return __atomic_load_n(&table_owner, __ATOMIC_ACQUIRE) == p; }
int giulia_wlan_pdc_check(const uint8_t *dt, uint32_t bytes,
    const struct giulia_wlan_pdc_baseline *baseline, unsigned policy)
{
    if (policy > GIULIA_WLAN_PDC_KEEP_TABLE) return GIULIA_AOP_ARGUMENT;
    if (!matches(dt, bytes)) return GIULIA_WLAN_PDC_TABLE_MISMATCH;
    if (policy == GIULIA_WLAN_PDC_RESTORE_KNOWN) {
        if (!baseline || baseline->known_mask != GIULIA_WLAN_PDC_ALL)
            return GIULIA_WLAN_PDC_BASELINE_UNKNOWN;
        for (unsigned i = 0; i < 10; i++)
            if (baseline->millivolts[i] > 8191u) return GIULIA_AOP_ARGUMENT;
    }
    return 0;
}
int giulia_wlan_pdc_recover(struct giulia_wlan_pdc *p)
{
    int rc;
    if (!p || !p->aop || !owns(p)) return GIULIA_AOP_STATE;
    if (p->aop->pending) {
        if ((rc = giulia_aop_recover(p->aop))) return rc;
        if (p->pending_index >= 10) return GIULIA_AOP_STATE;
        if (p->restoring) p->attempted &= ~(1u << p->pending_index);
        else p->delivered |= 1u << p->pending_index;
        p->pending_index = 10;
    }
    return p->aop->state == GIULIA_AOP_OPEN ? 0 : GIULIA_AOP_STATE;
}
int giulia_wlan_pdc_stop(struct giulia_wlan_pdc *p, uint32_t conditions)
{
    char text[64];
    int rc;
    if (!p) return GIULIA_AOP_ARGUMENT;
    if (p->state == GIULIA_WLAN_PDC_IDLE || p->state == GIULIA_WLAN_PDC_STOPPED) return 0;
    if ((conditions & GIULIA_WLAN_PDC_REQUIRED) != GIULIA_WLAN_PDC_REQUIRED)
        return GIULIA_AOP_PREREQUISITE;
    if (!owns(p)) return GIULIA_AOP_STATE;
    if (p->state == GIULIA_WLAN_PDC_HELD) return GIULIA_WLAN_PDC_RETAINED;
    if ((rc = giulia_wlan_pdc_recover(p))) goto failed;
    if (p->policy == GIULIA_WLAN_PDC_KEEP_TABLE && p->attempted) {
        p->state = GIULIA_WLAN_PDC_HELD; return GIULIA_WLAN_PDC_RETAINED;
    }
    p->restoring = 1;
    for (unsigned i = 10; i > 0; i--) {
        unsigned index = i - 1;
        if (!(p->attempted & (1u << index))) continue;
        message(index, p->baseline.millivolts[index], text);
        p->pending_index = index;
        rc = giulia_aop_send_text64(p->aop, text);
        if (rc) goto failed;
        p->attempted &= ~(1u << index);
        p->pending_index = 10;
    }
    struct giulia_wlan_pdc *expected = p;
    if (!__atomic_compare_exchange_n(&table_owner, &expected, 0, 0,
        __ATOMIC_RELEASE, __ATOMIC_RELAXED)) { rc = GIULIA_AOP_STATE; goto failed; }
    p->state = GIULIA_WLAN_PDC_STOPPED; p->cleanup_error = 0;
    return 0;
failed:
    p->state = GIULIA_WLAN_PDC_FAULT; p->cleanup_error = rc;
    return rc;
}
int giulia_wlan_pdc_start(struct giulia_wlan_pdc *p, struct giulia_aop *aop,
    const uint8_t *dt, uint32_t bytes, const struct giulia_wlan_pdc_baseline *baseline,
    uint32_t conditions, unsigned policy)
{
    struct giulia_wlan_pdc *empty = 0;
    char text[64];
    int rc;
    if (!p || !aop || policy > GIULIA_WLAN_PDC_KEEP_TABLE) return GIULIA_AOP_ARGUMENT;
    if (p->state != GIULIA_WLAN_PDC_IDLE && p->state != GIULIA_WLAN_PDC_STOPPED)
        return GIULIA_AOP_STATE;
    if ((conditions & GIULIA_WLAN_PDC_REQUIRED) != GIULIA_WLAN_PDC_REQUIRED)
        return GIULIA_AOP_PREREQUISITE;
    if ((rc = giulia_wlan_pdc_check(dt, bytes, baseline, policy))) return rc;
    if (aop->state != GIULIA_AOP_OPEN || !aop->owned || aop->pending) return GIULIA_AOP_STATE;
    if (!__atomic_compare_exchange_n(&table_owner, &empty, p, 0,
        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return GIULIA_AOP_BUSY;
    p->aop = aop; p->policy = policy; p->attempted = p->delivered = p->restoring = 0;
    p->pending_index = 10; p->error = p->cleanup_error = 0;
    if (baseline) p->baseline = *baseline;
    p->state = GIULIA_WLAN_PDC_FAULT;
    for (unsigned i = 0; i < 10; i++) {
        message(i, target[i], text); p->pending_index = i;
        rc = giulia_aop_send_text64(aop, text);
        if (!rc || aop->pending) p->attempted |= 1u << i;
        if (rc) { p->error = rc; return rc; }
        p->delivered |= 1u << i; p->pending_index = 10;
    }
    p->state = GIULIA_WLAN_PDC_DELIVERED;
    return 0;
}
