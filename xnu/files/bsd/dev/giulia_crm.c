/* SPDX-License-Identifier: BSD-2-Clause */
/* Protocol/layout reference: OnePlusOSS/android_kernel_oneplus_sm8650@
 * 66305c56af1fcd7299c906c3257e585be93d93ed/drivers/soc/qcom/crm.c:
 * crm_get_offset, crm_get_channel, crm_write_pwr_states, crm_switch_channel.
 * Original preservation/recovery implementation; no Linux source copied.
 */
#include "giulia_crm.h"
#define BUSY 0x1000u
#define UPDATE 0x1020u
static struct giulia_crm *owner;
static const uint32_t perf_off[5] = {0,0x40,0x80,0x600,0x640};
static const uint32_t bw_off[5] = {0xc0,0x140,0x680,0x700,0x780};
static int fail(struct giulia_crm *p, int rc)
{ p->state = GIULIA_CRM_FAULT; p->error = rc; return rc; }
static int lease(struct giulia_crm *p)
{
    p->callback_error = p->io.validate_lease(p->io.cookie);
    return p->callback_error ? GIULIA_CRM_LEASE : 0;
}
static int rd(struct giulia_crm *p, uint32_t off, uint32_t *v)
{
    p->callback_error = p->io.read32(p->io.cookie,off,v);
    return p->callback_error ? GIULIA_CRM_IO : 0;
}
static int wr(struct giulia_crm *p, uint32_t off, uint32_t v)
{
    uint32_t readback;
    int rc = lease(p);
    if (rc) return rc;
    p->callback_error = p->io.write32(p->io.cookie,off,v);
    p->io.barrier(p->io.cookie);
    if (p->callback_error) return GIULIA_CRM_IO;
    if ((rc = rd(p,off,&readback))) return rc;
    return readback == v ? 0 : GIULIA_CRM_DRIFT;
}
static uint32_t offset(struct giulia_crm *p, unsigned kind,
    unsigned ch, unsigned state, unsigned resource)
{
    unsigned count = kind == GIULIA_CRM_PERF ? p->perf_count : p->bw_count;
    return 0x200u + (kind == GIULIA_CRM_PERF ? perf_off[state] : bw_off[state])
        + (ch * count + resource)*4u;
}
static uint32_t *word(struct giulia_crm_table *t, unsigned kind, unsigned state, unsigned i)
{ return kind == GIULIA_CRM_PERF ? &t->perf[state][i] : &t->bw[state][i]; }
static int header(struct giulia_crm *p, int compare)
{
    uint32_t h[4]; int rc;
    if ((rc = lease(p))) return rc;
    for (unsigned i = 0; i < 4; i++) {
        if ((rc = rd(p,i*4,&h[i]))) return rc;
        if (h[i] == UINT32_MAX) return GIULIA_CRM_LAYOUT;
        if (compare && h[i] != p->header[i]) return GIULIA_CRM_DRIFT;
    }
    if (!compare) {
        unsigned perf = (h[1]>>8)&15u, bw = (h[2]>>26)&31u;
        /* Version observed on this Ace5; reject unvalidated geometry. */
        if (h[0] != 0x10000u || h[3] != 1 || ((h[1]>>4)&15u) != 2 ||
            (h[1]&15u) != 5 || !((h[1]>>16)&15u) ||
            !perf || perf > 8 || !bw || bw > 16) return GIULIA_CRM_LAYOUT;
        for (unsigned i = 0; i < 4; i++) p->header[i] = h[i];
        p->perf_count = perf; p->bw_count = bw;
    }
    return 0;
}
static int settled(struct giulia_crm *p, unsigned ch)
{
    uint32_t update,busy; int rc;
    if ((rc = header(p,1)) || (rc = rd(p,UPDATE,&update)) || (rc = rd(p,BUSY,&busy))) return rc;
    return update == (1u<<ch) && busy == (1u<<ch) ? 0 : GIULIA_CRM_BUSY;
}
static int table_read(struct giulia_crm *p, unsigned ch, int compare)
{
    uint32_t v; int rc;
    for (unsigned k = 0; k < 2; k++) for (unsigned s = 0; s < 5; s++) {
        unsigned count = k ? p->bw_count : p->perf_count;
        for (unsigned i = 0; i < count; i++) {
            if ((rc = rd(p,offset(p,k,ch,s,i),&v))) return rc;
            if (v == UINT32_MAX) return GIULIA_CRM_IO;
            uint32_t *saved = word(&p->saved[ch],k,s,i);
            if (compare && v != *saved) return GIULIA_CRM_DRIFT;
            if (!compare) *saved = v;
        }
    }
    return 0;
}
static int table_write(struct giulia_crm *p, unsigned ch,
    const struct giulia_crm_patch *patch, unsigned n, int restore)
{
    int rc;
    for (unsigned k = 0; k < 2; k++) for (unsigned s = 0; s < 5; s++) {
        unsigned count = k ? p->bw_count : p->perf_count;
        for (unsigned i = 0; i < count; i++) {
            uint32_t v = *word(&p->saved[restore ? ch : p->original],k,s,i);
            for (unsigned j = 0; j < n; j++)
                if (patch[j].kind == k && patch[j].state == s && patch[j].resource == i)
                    v = (v & ~patch[j].mask) | patch[j].value;
            p->dirty = 1; /* write callbacks may fail AFTER issuing a write. */
            if ((rc = wr(p,offset(p,k,ch,s,i),v))) return rc;
        }
    }
    return 0;
}
int giulia_crm_recover(struct giulia_crm *p)
{
    uint32_t update,busy; int rc;
    if (!p || !p->owned || __atomic_load_n(&owner,__ATOMIC_ACQUIRE) != p)
        return GIULIA_CRM_STATE;
    if (!p->pending) return 0;
    for (unsigned i = 0; i < 100; i++) {
        if ((rc = header(p,1)) || (rc = rd(p,UPDATE,&update)) || (rc = rd(p,BUSY,&busy))) return rc;
        if (update != (1u<<p->pending_channel)) return GIULIA_CRM_DRIFT;
        if (busy == (1u<<p->pending_channel)) { p->pending = 0; return 0; }
        if (busy & ~3u) return GIULIA_CRM_DRIFT;
        p->io.delay_us(p->io.cookie,100);
    }
    return GIULIA_CRM_TIMEOUT;
}
static int request(struct giulia_crm *p, unsigned ch)
{
    int rc = lease(p);
    if (rc) return rc;
    p->pending = 1; p->pending_channel = ch;
    p->callback_error = p->io.write32(p->io.cookie,UPDATE,1u<<ch);
    p->io.barrier(p->io.cookie);
    if (p->callback_error) return GIULIA_CRM_IO;
    return giulia_crm_recover(p);
}
int giulia_crm_stop(struct giulia_crm *p)
{
    int rc;
    if (!p) return GIULIA_CRM_ARGUMENT;
    if (p->state == GIULIA_CRM_IDLE || p->state == GIULIA_CRM_STOPPED) return 0;
    if (!p->owned || __atomic_load_n(&owner,__ATOMIC_ACQUIRE) != p) return GIULIA_CRM_STATE;
    if ((rc = giulia_crm_recover(p))) goto failed;
    if (p->dirty) {
        /* Original table was never ours to edit. Validate before selecting it. */
        if ((rc = header(p,1)) || (rc = table_read(p,p->original,1))) goto failed;
        rc = settled(p,p->original);
        if (rc == GIULIA_CRM_BUSY) {
            if ((rc = settled(p,p->target)) || (rc = request(p,p->original))) goto failed;
        } else if (rc) goto failed;
        if ((rc = settled(p,p->original)) || (rc = table_write(p,p->target,0,0,1)) ||
            (rc = table_read(p,p->target,1)) || (rc = settled(p,p->original))) goto failed;
        p->dirty = 0;
    }
    struct giulia_crm *expected = p;
    if (!__atomic_compare_exchange_n(&owner,&expected,0,0,__ATOMIC_RELEASE,__ATOMIC_RELAXED)) {
        rc = GIULIA_CRM_STATE; goto failed;
    }
    p->owned = 0; p->state = GIULIA_CRM_STOPPED; p->cleanup_error = 0;
    return 0;
failed:
    p->state = GIULIA_CRM_FAULT; p->cleanup_error = rc; return rc;
}
int giulia_crm_start(struct giulia_crm *p, const struct giulia_crm_io *io,
    const struct giulia_crm_patch *patch, unsigned n)
{
    struct giulia_crm *empty = 0;
    uint32_t update,busy; int rc;
    if (!p || !io || !io->read32 || !io->write32 || !io->barrier ||
        !io->delay_us || !io->validate_lease || !patch || !n || n > 64) return GIULIA_CRM_ARGUMENT;
    if (p->state != GIULIA_CRM_IDLE && p->state != GIULIA_CRM_STOPPED) return GIULIA_CRM_STATE;
    for (unsigned i = 0; i < n; i++) {
        uint32_t allowed = patch[i].kind == GIULIA_CRM_PERF ? 7u : GIULIA_CRM_BW_FIELDS;
        if (patch[i].kind > GIULIA_CRM_BW || patch[i].state >= 5 || !patch[i].mask ||
            (patch[i].mask & ~allowed) || (patch[i].value & ~patch[i].mask)) return GIULIA_CRM_ARGUMENT;
        for (unsigned j = 0; j < i; j++)
            if (patch[i].kind == patch[j].kind && patch[i].state == patch[j].state &&
                patch[i].resource == patch[j].resource && (patch[i].mask & patch[j].mask))
                return GIULIA_CRM_ARGUMENT;
    }
    if (!__atomic_compare_exchange_n(&owner,&empty,p,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))
        return GIULIA_CRM_BUSY;
    p->io = *io; p->owned = 1; p->dirty = p->pending = p->error = p->cleanup_error = 0;
    p->state = GIULIA_CRM_PREPARING;
    if ((rc = header(p,0)) || (rc = rd(p,UPDATE,&update)) || (rc = rd(p,BUSY,&busy))) goto rejected;
    if ((update != 1 && update != 2) || busy != update) { rc = GIULIA_CRM_BUSY; goto rejected; }
    p->original = update == 1 ? 0 : 1; p->target = 1-p->original;
    for (unsigned i = 0; i < n; i++)
        if (patch[i].resource >= (patch[i].kind == GIULIA_CRM_PERF ? p->perf_count : p->bw_count)) {
            rc = GIULIA_CRM_ARGUMENT; goto rejected;
        }
    if ((rc = table_read(p,0,0)) || (rc = table_read(p,1,0)) ||
        (rc = settled(p,p->original)) || (rc = table_read(p,p->original,1))) goto rejected;
    if ((rc = table_write(p,p->target,patch,n,0)) ||
        (rc = settled(p,p->original)) || (rc = table_read(p,p->original,1)) ||
        (rc = request(p,p->target))) return fail(p,rc);
    p->state = GIULIA_CRM_ACTIVE; return 0;
rejected:
    p->error = rc;
    /* No writes occurred. Release the software owner, not any platform lease. */
    struct giulia_crm *expected = p;
    (void)__atomic_compare_exchange_n(&owner,&expected,0,0,__ATOMIC_RELEASE,__ATOMIC_RELAXED);
    p->owned = 0; p->state = GIULIA_CRM_STOPPED; return rc;
}
