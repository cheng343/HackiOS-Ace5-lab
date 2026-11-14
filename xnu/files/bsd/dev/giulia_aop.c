/* SPDX-License-Identifier: BSD-2-Clause */
#include "giulia_aop.h"
#define MAGIC 0x4d41494cu
#define UL 0x0cu
#define ULA 0x10u
#define UC 0x14u
#define UCA 0x18u
#define ML 0x24u
#define MLA 0x28u
#define MC 0x2cu
#define MCA 0x30u
#define MS 0x34u
#define MO 0x38u
static struct giulia_aop *owner;

int giulia_aop_inspect(const struct giulia_aop_io *io, uint32_t bytes,
    struct giulia_aop_snapshot *s)
{
    uint32_t i, offset, size;
    if (!io || !io->read32 || !s || bytes < 0x40 || bytes > 0x10000 || (bytes & 3u))
        return GIULIA_AOP_ARGUMENT;
    *s = (struct giulia_aop_snapshot){0};
    for (i = 0; i < 15; i++) s->words[i] = io->read32(io->cookie, i * 4u);
    offset = s->words[MO/4]; size = s->words[MS/4];
    if (s->words[0] != MAGIC || s->words[1] != 1 || (offset & 3u) ||
        (size & 3u) || offset < 0x3c || offset > bytes || size < 8 || size > bytes - offset)
        return GIULIA_AOP_LAYOUT;
    s->message_length = io->read32(io->cookie, offset);
    /* This is a changing shared descriptor; report drift rather than invent
     * an atomic capture. Never follow a newly changed mailbox offset. */
    for (i = 0; i < 15; i++)
        if (s->words[i] != io->read32(io->cookie, i * 4u)) s->changed = 1;
    return GIULIA_AOP_OK;
}

static int fail(struct giulia_aop *p, int error)
{ p->state = GIULIA_AOP_FAULT; p->error = error; return error; }
static int claim(struct giulia_aop *p)
{
    struct giulia_aop *empty = 0;
    return __atomic_compare_exchange_n(&owner, &empty, p, 0,
        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) ? 0 : GIULIA_AOP_BUSY;
}
static void release(struct giulia_aop *p)
{
    struct giulia_aop *expected = p;
    (void)__atomic_compare_exchange_n(&owner, &expected, 0, 0,
        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    p->owned = 0;
}
static int write_word(struct giulia_aop *p, uint32_t off, uint32_t value)
{
    p->io.write32(p->io.cookie, off, value);
    p->io.barrier(p->io.cookie);
    return p->io.read32(p->io.cookie, off) == value ? 0 : GIULIA_AOP_READBACK;
}
static int kick(struct giulia_aop *p)
{
    p->io.barrier(p->io.cookie);
    return p->io.kick(p->io.cookie) ? GIULIA_AOP_DOORBELL : 0;
}
static int wait_word(struct giulia_aop *p, uint32_t off, uint32_t value)
{
    unsigned n;
    for (n = 0; n < 10000; n++) {
        uint32_t v = p->io.read32(p->io.cookie, off);
        ++p->polls;
        if (v == value) return 0;
        if (v == 0xffffffffu) return GIULIA_AOP_READBACK;
        p->io.delay_us(p->io.cookie, 100);
    }
    return GIULIA_AOP_TIMEOUT;
}
static int layout(struct giulia_aop *p, int existing)
{
    uint32_t magic = p->io.read32(p->io.cookie, 0);
    uint32_t version = p->io.read32(p->io.cookie, 4);
    uint32_t size = p->io.read32(p->io.cookie, MS);
    uint32_t offset = p->io.read32(p->io.cookie, MO);
    if (magic != MAGIC || version != 1 || (offset & 3u) || (size & 3u) ||
        offset < 0x3cu || offset > p->bytes || size < 8u || size > p->bytes - offset)
        return GIULIA_AOP_LAYOUT;
    if (existing && (offset != p->offset || size != p->size))
        return GIULIA_AOP_LAYOUT;
    p->offset = offset; p->size = size;
    return 0;
}
static int channel_up(struct giulia_aop *p)
{
    static const uint32_t offsets[] = {UL, ULA, UC, UCA, ML, MLA, MC, MCA};
    unsigned i;
    int rc = layout(p, 1);
    if (rc) return rc;
    for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        uint32_t v = p->io.read32(p->io.cookie, offsets[i]);
        if (v == 0xffffffffu) return GIULIA_AOP_READBACK;
        if (v != GIULIA_AOP_UP) return GIULIA_AOP_BUSY;
    }
    return 0;
}
int giulia_aop_open(struct giulia_aop *p, const struct giulia_aop_io *io,
    uint32_t bytes, uint32_t flags, unsigned policy)
{
    int rc;
    if (!p || !io || !io->read32 || !io->write32 || !io->barrier ||
        !io->delay_us || !io->kick || bytes < 0x40 || bytes > 0x10000 ||
        (bytes & 3u) || policy > GIULIA_AOP_OPEN_ON_INHERITED_LINK) return GIULIA_AOP_ARGUMENT;
    if ((flags & GIULIA_AOP_REQUIRED) != GIULIA_AOP_REQUIRED ||
        (policy != GIULIA_AOP_OPEN_COLD && !(flags & GIULIA_AOP_INHERITED_CHANNEL_LEASE)))
        return GIULIA_AOP_PREREQUISITE;
    if (p->state != GIULIA_AOP_IDLE && p->state != GIULIA_AOP_STOPPED)
        return GIULIA_AOP_STATE;
    if ((rc = claim(p))) return rc;
    p->io = *io; p->bytes = bytes; p->polls = p->pending = p->error = 0;
    p->borrowed = policy == GIULIA_AOP_BORROW_OPEN; p->owned = 1;
    p->keep_link = policy == GIULIA_AOP_OPEN_ON_INHERITED_LINK;
    if ((rc = layout(p, 0))) goto rejected;
    if (p->io.read32(p->io.cookie, p->offset) != 0) {
        rc = GIULIA_AOP_BUSY; goto rejected;
    }
    if (p->borrowed) {
        if ((rc = channel_up(p))) goto rejected;
        p->state = GIULIA_AOP_OPEN; return 0;
    }
    if (p->keep_link) {
        uint32_t uc = p->io.read32(p->io.cookie, UC);
        uint32_t uca = p->io.read32(p->io.cookie, UCA);
        if (p->io.read32(p->io.cookie, UL) != GIULIA_AOP_UP ||
            p->io.read32(p->io.cookie, ULA) != GIULIA_AOP_UP ||
            p->io.read32(p->io.cookie, ML) != GIULIA_AOP_UP ||
            p->io.read32(p->io.cookie, MLA) != GIULIA_AOP_UP ||
            p->io.read32(p->io.cookie, MC) != GIULIA_AOP_DOWN ||
            p->io.read32(p->io.cookie, MCA) != GIULIA_AOP_DOWN ||
            (uc != GIULIA_AOP_UP && uc != GIULIA_AOP_DOWN) ||
            (uca != GIULIA_AOP_UP && uca != GIULIA_AOP_DOWN)) {
            rc = GIULIA_AOP_BUSY; goto rejected;
        }
        p->state = GIULIA_AOP_FAULT;
        goto open_channel;
    }
    if (p->io.read32(p->io.cookie, UL) != GIULIA_AOP_UP ||
        p->io.read32(p->io.cookie, ML) != GIULIA_AOP_DOWN ||
        p->io.read32(p->io.cookie, MLA) != GIULIA_AOP_DOWN ||
        p->io.read32(p->io.cookie, MC) != GIULIA_AOP_DOWN ||
        p->io.read32(p->io.cookie, MCA) != GIULIA_AOP_DOWN ||
        p->io.read32(p->io.cookie, UC) != GIULIA_AOP_DOWN) {
        rc = GIULIA_AOP_BUSY; goto rejected;
    }
    p->state = GIULIA_AOP_FAULT;
    if ((rc = write_word(p, ULA, GIULIA_AOP_UP)) ||
        (rc = write_word(p, ML, GIULIA_AOP_UP)) || (rc = kick(p)) ||
        (rc = wait_word(p, MLA, GIULIA_AOP_UP))) return fail(p, rc);
open_channel:
    if ((rc = write_word(p, MC, GIULIA_AOP_UP)) || (rc = kick(p)) ||
        (rc = wait_word(p, UC, GIULIA_AOP_UP)) ||
        (rc = write_word(p, UCA, GIULIA_AOP_UP)) || (rc = kick(p)) ||
        (rc = wait_word(p, MCA, GIULIA_AOP_UP)) || (rc = channel_up(p)))
        return fail(p, rc);
    p->state = GIULIA_AOP_OPEN; return 0;
rejected:
    release(p); p->error = rc; return rc;
}
int giulia_aop_send(struct giulia_aop *p, const uint8_t *data, uint32_t len)
{
    uint32_t i, v;
    int rc;
    if (!p || !data || !len || (len & 3u)) return GIULIA_AOP_ARGUMENT;
    if (p->state != GIULIA_AOP_OPEN || !p->owned || p->pending)
        return GIULIA_AOP_STATE;
    if (len > p->size - 4u) return GIULIA_AOP_ARGUMENT;
    if ((rc = channel_up(p))) return fail(p, rc);
    if (p->io.read32(p->io.cookie, p->offset) != 0) return fail(p, GIULIA_AOP_BUSY);
    for (i = 0; i < len; i += 4) {
        v = (uint32_t)data[i] | (uint32_t)data[i+1] << 8 |
            (uint32_t)data[i+2] << 16 | (uint32_t)data[i+3] << 24;
        if ((rc = write_word(p, p->offset + 4u + i, v))) return fail(p, rc);
    }
    /* Publish only after every payload word is present. An immediate 0 can
     * already be a firmware consumption; it cannot distinguish a dropped
     * self-clearing length write. This is the QMP protocol's ACK limitation. */
    p->pending = 1;
    p->io.write32(p->io.cookie, p->offset, len);
    p->io.barrier(p->io.cookie);
    v = p->io.read32(p->io.cookie, p->offset);
    if (v != len && v != 0) return fail(p, GIULIA_AOP_READBACK);
    if ((rc = kick(p)) || (rc = wait_word(p, p->offset, 0))) return fail(p, rc);
    if ((rc = channel_up(p))) return fail(p, rc);
    p->pending = 0; p->error = 0; return 0;
}
int giulia_aop_send_text64(struct giulia_aop *p, const char *text)
{
    uint8_t data[64] = {0};
    unsigned i;
    if (!text) return GIULIA_AOP_ARGUMENT;
    for (i = 0; i < 64 && text[i]; i++) data[i] = (uint8_t)text[i];
    if (!i || i == 64) return GIULIA_AOP_ARGUMENT;
    return giulia_aop_send(p, data, sizeof(data));
}
int giulia_aop_recover(struct giulia_aop *p)
{
    int rc;
    if (!p) return GIULIA_AOP_ARGUMENT;
    if (p->state != GIULIA_AOP_FAULT || !p->owned || !p->pending)
        return GIULIA_AOP_STATE;
    if ((rc = channel_up(p)) || (rc = wait_word(p, p->offset, 0))) return fail(p, rc);
    p->pending = 0; p->error = 0; p->state = GIULIA_AOP_OPEN;
    return 0;
}
int giulia_aop_close(struct giulia_aop *p)
{
    int rc;
    if (!p) return GIULIA_AOP_ARGUMENT;
    if (p->state == GIULIA_AOP_IDLE || p->state == GIULIA_AOP_STOPPED) return 0;
    if (!p->owned) return GIULIA_AOP_STATE;
    if (p->pending) return GIULIA_AOP_BUSY;
    if ((rc = layout(p, 1))) return fail(p, rc);
    if (p->io.read32(p->io.cookie, p->offset) != 0) return fail(p, GIULIA_AOP_BUSY);
    if (!p->borrowed) {
        if ((rc = write_word(p, MC, GIULIA_AOP_DOWN))) return fail(p, rc);
        if (!p->keep_link && (rc = write_word(p, ML, GIULIA_AOP_DOWN))) return fail(p, rc);
        if ((rc = kick(p)) || (rc = wait_word(p, MCA, GIULIA_AOP_DOWN))) return fail(p, rc);
        if (!p->keep_link && (rc = wait_word(p, MLA, GIULIA_AOP_DOWN))) return fail(p, rc);
    }
    p->state = GIULIA_AOP_STOPPED; p->error = 0; release(p);
    return 0;
}
