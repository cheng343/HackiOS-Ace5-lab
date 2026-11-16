/* SPDX-License-Identifier: BSD-2-Clause */
/* Independently implemented transport. Fixed public register/protocol facts:
 * https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/blob/66305c56af1fcd7299c906c3257e585be93d93ed/drivers/soc/qcom/rpmh-rsc.c
 * rpmh_rsc_reg_offsets_ver_3_0, __tcs_set_trigger, __tcs_buffer_write,
 * tcs_tx_done and rpmh_rsc_debug_tcs (ISSUED/COMPL diagnostic bits).
 * https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/blob/66305c56af1fcd7299c906c3257e585be93d93ed/drivers/soc/qcom/cmd-db.c
 * cmd_db_header/rsc_hdr/entry_header layout and slave/address query.
 * Resource READ protocol independently implemented from the public wire facts
 * in Qualcomm's upstream change, not copied from its Linux implementation:
 * https://github.com/torvalds/linux/commit/edbafe65eef2b58625db1e113fbbfb1fe10c0291
 * WRITE bit clear, one command, no DATA write, fresh completion before reading
 * RESP_DATA. Merely reading a prior TCS buffer is not a resource read request.
 */
#include "giulia_rpmh.h"
#define BIT(n) (1u << (n))
#define TCS 0xd00u
#define STRIDE 672u
#define IRQ_ENABLE (TCS + 0x00u)
#define IRQ_STATUS (TCS + 0x04u)
#define IRQ_CLEAR (TCS + 0x08u)
#define WAIT (TCS + 0x20u)
#define CONTROL (TCS + 0x24u)
#define STATUS (TCS + 0x28u)
#define ENABLE (TCS + 0x2cu)
#define MSG (TCS + 0x34u)
#define CMD_STATUS (TCS + 0x40u)
#define RESPONSE (TCS + 0x44u)
#define AMC BIT(16)
#define TRIGGER BIT(24)

static uintptr_t bus_owner;
int giulia_rpmh_bus_claim(const void *owner)
{
    uintptr_t expected = 0;
    if (!owner) return GIULIA_RPMH_ARGUMENT;
    return __atomic_compare_exchange_n(&bus_owner, &expected, (uintptr_t)owner,
        0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ? 0 : GIULIA_RPMH_BUSY;
}
int giulia_rpmh_bus_release(const void *owner)
{
    uintptr_t expected = (uintptr_t)owner;
    if (!owner) return GIULIA_RPMH_ARGUMENT;
    return __atomic_compare_exchange_n(&bus_owner, &expected, 0,
        0, __ATOMIC_RELEASE, __ATOMIC_RELAXED) ? 0 : GIULIA_RPMH_STATE;
}
int giulia_rpmh_bus_owned(const void *owner)
{
    return owner && __atomic_load_n(&bus_owner, __ATOMIC_ACQUIRE) == (uintptr_t)owner;
}
static uint32_t slot_offset(struct giulia_rpmh *p,uint32_t off)
{
    /* Admission scans with slot=0. Afterwards only the borrowed TCS bank
     * moves; shared DRV IRQ/version/config registers keep their addresses. */
    return off>=WAIT && off<TCS+STRIDE ? off+p->slot*STRIDE:off;
}
static uint32_t rd(struct giulia_rpmh *p, uint32_t off)
{ return p->io.read32(p->io.cookie, slot_offset(p,off)); }
static void wr(struct giulia_rpmh *p, uint32_t off, uint32_t value)
{
    p->io.write32(p->io.cookie, slot_offset(p,off), value);
    p->io.barrier(p->io.cookie);
}
static int sync_write(struct giulia_rpmh *p, uint32_t off, uint32_t value)
{
    unsigned i;
    wr(p, off, value);
    for (i = 0; i < 100; i++) {
        uint32_t got = rd(p, off);
        if (got == 0xffffffffu) return GIULIA_RPMH_READBACK;
        if (got == value) return 0;
        p->io.delay_us(p->io.cookie, 10);
    }
    return GIULIA_RPMH_READBACK;
}
static int irq_bit(struct giulia_rpmh *p, uint32_t bit)
{
    uint32_t old = rd(p, IRQ_ENABLE);
    if (old == 0xffffffffu) return GIULIA_RPMH_READBACK;
    return sync_write(p, IRQ_ENABLE, (old & ~BIT(p->slot)) | bit);
}
static int cleanup(struct giulia_rpmh *p)
{
    unsigned i, j;
    int r;
    uint32_t v;
    if (!p->dirty) return 0;
    /* A triggered TCS is never overwritten until completion and idle have
     * both been observed. A pre-trigger failure is safe to unwind directly.
     */
    if (p->triggered && !p->completed) return GIULIA_RPMH_BUSY;
    v = rd(p, STATUS);
    if (v == 0xffffffffu) return GIULIA_RPMH_READBACK;
    if (!v) return GIULIA_RPMH_BUSY;
    v = rd(p, CONTROL);
    if (v == 0xffffffffu) return GIULIA_RPMH_READBACK;
    if ((r = sync_write(p, CONTROL, v & ~TRIGGER)) ||
        (r = sync_write(p, CONTROL, v & ~(TRIGGER | AMC))) ||
        (r = sync_write(p, ENABLE, 0))) return r;
    if (p->triggered) {
        wr(p, IRQ_CLEAR, BIT(p->slot));
        v = rd(p, IRQ_STATUS);
        if (v == 0xffffffffu || (v & BIT(p->slot))) return GIULIA_RPMH_READBACK;
    }
    for (i = 0; i < p->count; i++)
        for (j = 0; j < (p->read_request ? 2u : 3u); j++)
            if ((r = sync_write(p, MSG + i * 24 + j * 4, p->saved_cmd[i][j])))
                return r;
    if ((r = sync_write(p, WAIT, p->saved_wait)) ||
        (r = irq_bit(p, p->saved_irq_bit)) ||
        (r = sync_write(p, CONTROL, p->saved_control))) return r;
    p->dirty = 0;
    return 0;
}
static int wait_done(struct giulia_rpmh *p)
{
    unsigned n, i;
    for (n = 0; n < 2000; n++) {
        int all_done = 1;
        p->polls++;
        p->last_irq = rd(p, IRQ_STATUS);
        p->last_tcs = rd(p, STATUS);
        if (p->last_irq == 0xffffffffu || p->last_tcs == 0xffffffffu)
            return GIULIA_RPMH_READBACK;
        if ((p->last_irq & BIT(p->slot)) && p->last_tcs) {
            for (i = 0; i < p->count; i++) {
                p->last_command = rd(p, CMD_STATUS + i * 24);
                if (p->last_command == 0xffffffffu) return GIULIA_RPMH_READBACK;
                if ((p->last_command & (BIT(8) | BIT(16))) != (BIT(8) | BIT(16)))
                    all_done = 0;
            }
            if (all_done) { p->completed = 1; return 0; }
        }
        p->io.delay_us(p->io.cookie, 100);
    }
    return GIULIA_RPMH_TIMEOUT;
}
int giulia_rpmh_init(struct giulia_rpmh *p, const struct giulia_rpmh_io *io, uint32_t flags)
{
    struct giulia_rpmh_io copy;
    if (!p || !io || !io->read32 || !io->write32 || !io->barrier || !io->delay_us)
        return GIULIA_RPMH_ARGUMENT;
    if (p->state != GIULIA_RPMH_IDLE && p->state != GIULIA_RPMH_DONE)
        return GIULIA_RPMH_STATE;
    if ((flags & GIULIA_RPMH_REQUIRED) != GIULIA_RPMH_REQUIRED)
        return GIULIA_RPMH_PREREQUISITE;
    copy = *io;
    *p = (struct giulia_rpmh){0}; p->io = copy; p->prerequisites = flags;
    return 0;
}
int giulia_rpmh_recover(struct giulia_rpmh *p)
{
    int r;
    if (!p) return GIULIA_RPMH_ARGUMENT;
    if (p->state == GIULIA_RPMH_IDLE || p->state == GIULIA_RPMH_DONE) return 0;
    if (!giulia_rpmh_bus_owned(p)) return GIULIA_RPMH_STATE;
    if (p->triggered && !p->completed && (r = wait_done(p))) goto fault;
    if (p->read_request && p->completed && !p->response_valid) {
        uint32_t first, second;
        /* Capture before cleanup acknowledges or reprograms this TCS. Keep
         * the lease on a bad read so recovery can inspect the same response.
         * Never retrigger a READ merely because result capture failed. */
        p->io.barrier(p->io.cookie);
        first = rd(p, RESPONSE);
        p->io.barrier(p->io.cookie);
        second = rd(p, RESPONSE);
        if (first == 0xffffffffu || first != second) {
            r = GIULIA_RPMH_READBACK; goto fault;
        }
        p->response = first; p->response_valid = 1;
    }
    if ((r = cleanup(p))) goto fault;
    r = giulia_rpmh_bus_release(p);
    if (r) goto fault;
    p->state = GIULIA_RPMH_DONE; p->cleanup_error = 0;
    return 0;
fault:
    p->cleanup_error = r; p->state = GIULIA_RPMH_FAULT;
    return r;
}
static int transfer(struct giulia_rpmh *p, const struct giulia_rpmh_command *cmd,
    unsigned count, unsigned is_read)
{
    unsigned i, j, selected=3;
    uint32_t first_status=0,first_enable=0,first_control=0;
    uint32_t v, config;
    int r;
    if (!p || !cmd || !count || count > GIULIA_RPMH_MAX_COMMANDS || (is_read && count != 1))
        return GIULIA_RPMH_ARGUMENT;
    if ((p->prerequisites & GIULIA_RPMH_REQUIRED) != GIULIA_RPMH_REQUIRED)
        return GIULIA_RPMH_PREREQUISITE;
    if (p->state != GIULIA_RPMH_IDLE && p->state != GIULIA_RPMH_DONE)
        return GIULIA_RPMH_STATE;
    for (i = 0; i < count; i++) {
        if (!cmd[i].address || (cmd[i].address & 3u) || (!is_read && cmd[i].data == 0xffffffffu))
            return GIULIA_RPMH_ARGUMENT;
        for (j = 0; j < i; j++)
            if (cmd[i].address == cmd[j].address) return GIULIA_RPMH_ARGUMENT;
    }
    /* A new admitted attempt must not expose an earlier transfer's outcome
     * when it loses the bus lease before touching MMIO.
     */
    p->triggered = p->completed = 0;
    p->response = p->response_valid = 0; p->read_request = is_read;
    p->slot = 0;
    p->busy_reason = p->busy_irq = p->busy_slot = 0;
    p->busy_status = p->busy_enable = p->busy_control = 0;
    if ((r = giulia_rpmh_bus_claim(p))) {
        p->busy_reason = GIULIA_RPMH_BUSY_OWNER; return r;
    }
    p->state = GIULIA_RPMH_OWNED; p->count = count;
    p->dirty = p->triggered = p->completed = p->polls = 0;
    p->error = p->cleanup_error = 0;
    v = rd(p, 0); config = rd(p, 0x0c);
    if (v == 0xffffffffu || config == 0xffffffffu) { r = GIULIA_RPMH_READBACK; goto failed; }
    if (((v >> 16) & 255u) != 3u || ((config >> 12) & 63u) < 3u ||
        ((config >> 27) & 31u) < count) { r = GIULIA_RPMH_PREREQUISITE; goto failed; }
    v = rd(p, IRQ_STATUS);
    if (v == 0xffffffffu) { r = GIULIA_RPMH_READBACK; goto failed; }
    if (v & 7u) {
        p->busy_reason = GIULIA_RPMH_BUSY_IRQ; p->busy_irq = v;
        r = GIULIA_RPMH_BUSY; goto failed;
    }
    /* Refuse another active-slot transaction, including overlapping VRM
     * resources. Global lease serializes all cooperative clients thereafter.
     */
    for (i = 0; i < 3; i++) {
        uint32_t status = rd(p, STATUS + i * STRIDE);
        uint32_t enable = rd(p, ENABLE + i * STRIDE);
        uint32_t control = rd(p, CONTROL + i * STRIDE);
        if (status == 0xffffffffu || enable == 0xffffffffu || control == 0xffffffffu)
            { r = GIULIA_RPMH_READBACK; goto failed; }
        if (!i) { first_status=status; first_enable=enable; first_control=control; }
        if (!status || (!is_read && (enable || (control & TRIGGER)))) {
            p->busy_reason = GIULIA_RPMH_BUSY_SLOT; p->busy_slot = i;
            p->busy_status = status; p->busy_enable = enable; p->busy_control = control;
            r = GIULIA_RPMH_BUSY; goto failed;
        }
        if (!enable && !(control&TRIGGER) && selected==3) selected=i;
    }
    if (is_read && selected==3) {
        p->busy_reason=GIULIA_RPMH_BUSY_SLOT; p->busy_slot=0;
        p->busy_status=first_status; p->busy_enable=first_enable; p->busy_control=first_control;
        r=GIULIA_RPMH_BUSY; goto failed;
    }
    if (is_read) p->slot=selected;
    p->saved_control = rd(p, CONTROL); p->saved_wait = rd(p, WAIT);
    v = rd(p, IRQ_ENABLE);
    if (p->saved_control == 0xffffffffu || p->saved_wait == 0xffffffffu || v == 0xffffffffu)
        { r = GIULIA_RPMH_READBACK; goto failed; }
    p->saved_irq_bit = v & BIT(p->slot);
    for (i = 0; i < count; i++) for (j = 0; j < (is_read ? 2u : 3u); j++) {
        p->saved_cmd[i][j] = rd(p, MSG + i * 24 + j * 4);
        if (p->saved_cmd[i][j] == 0xffffffffu) { r = GIULIA_RPMH_READBACK; goto failed; }
    }
    p->dirty = 1;
    if ((r = sync_write(p, CONTROL, p->saved_control & ~TRIGGER)) ||
        (r = sync_write(p, CONTROL, p->saved_control & ~(TRIGGER | AMC)))) goto failed;
    for (i = 0; i < count; i++) {
        if ((r = sync_write(p, MSG + i * 24, is_read ? 0x108u : 0x10108u)) ||
            (r = sync_write(p, MSG + i * 24 + 4, cmd[i].address)) ||
            (!is_read && (r = sync_write(p, MSG + i * 24 + 8, cmd[i].data)))) goto failed;
    }
    v = (1u << count) - 1u;
    if ((r = sync_write(p, WAIT, v)) || (r = sync_write(p, ENABLE, v)) ||
        (r = irq_bit(p, BIT(p->slot))) || (r = sync_write(p, CONTROL, AMC))) goto failed;
    p->triggered = 1; /* Even a lost read after this point may mean it issued. */
    wr(p, CONTROL, AMC | TRIGGER);
    if ((r = wait_done(p))) goto failed;
    return giulia_rpmh_recover(p);
failed:
    p->error = r; p->state = GIULIA_RPMH_FAULT;
    if (!p->triggered) (void)giulia_rpmh_recover(p);
    return r;
}

int giulia_rpmh_write(struct giulia_rpmh *p, const struct giulia_rpmh_command *cmd, unsigned count)
{ return transfer(p, cmd, count, 0); }

int giulia_rpmh_read(struct giulia_rpmh *p, uint32_t address)
{
    const struct giulia_rpmh_command cmd = {address, 0};
    return transfer(p, &cmd, 1, 1);
}

int giulia_rpmh_read_result(const struct giulia_rpmh *p, uint32_t *value)
{
    if (!p || !value) return GIULIA_RPMH_ARGUMENT;
    if (p->state != GIULIA_RPMH_DONE || !p->read_request || !p->completed ||
        !p->response_valid || p->dirty || giulia_rpmh_bus_owned(p)) return GIULIA_RPMH_STATE;
    *value = p->response;
    return 0;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
int giulia_rpmh_resolve_vrm(const uint8_t *db, uint32_t bytes,
    uint32_t expected_version, const char *name, uint32_t *address)
{
    uint8_t key[8] = {0};
    uint32_t found = 0;
    unsigned len, s, i, j, matches = 0, ended = 0;
    if (!db || !name || !address || !expected_version || expected_version == 0xffffffffu)
        return GIULIA_RPMH_ARGUMENT;
    *address = 0;
    for (len = 0; len < 8 && name[len]; len++) key[len] = (uint8_t)name[len];
    if (!len || (len == 8 && name[8])) return GIULIA_RPMH_ARGUMENT;
    if (bytes < 144 || bytes > GIULIA_RPMH_DB_MAX || le32(db) != expected_version ||
        le32(db + 4) != 0x0c0330dbu) return GIULIA_RPMH_DATABASE;
    for (s = 0; s < 8; s++) {
        const uint8_t *desc = db + 8 + s * 16;
        uint32_t slave = le16(desc), off = le16(desc + 2), aux = le16(desc + 4);
        unsigned count = le16(desc + 6);
        if (!slave) {
            if (count) return GIULIA_RPMH_DATABASE;
            ended = 1;
            continue;
        }
        /* Official lookup stops at the first zero slave descriptor. Do not
         * accept a resource hidden behind that terminator in malformed data.
         */
        if (ended || slave > 7 || count > 512 || off > bytes - 144 ||
            count * 24u > bytes - 144 - off || aux > bytes - 144)
            return GIULIA_RPMH_DATABASE;
        for (i = 0; i < count; i++) {
            const uint8_t *e = db + 144 + off + i * 24;
            uint32_t alen = le16(e + 20), aoff = le16(e + 22);
            int same = 1;
            if (aoff > bytes - 144 - aux || alen > bytes - 144 - aux - aoff)
                return GIULIA_RPMH_DATABASE;
            for (j = 0; j < 8; j++) if (e[j] != key[j]) same = 0;
            if (!same) continue;
            found = le32(e + 16);
            if (++matches != 1 || slave != 4 || ((found >> 16) & 15u) != 4 ||
                (found & 15u) || found > 0xfffffff0u)
                return GIULIA_RPMH_DATABASE;
        }
    }
    if (!matches) return GIULIA_RPMH_NOT_FOUND;
    *address = found;
    return 0;
}
