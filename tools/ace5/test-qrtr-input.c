/* Actual QRTR input parser with only transport/callback services replaced. */
#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qrtr-types.inc"
_Static_assert(sizeof(struct qrtr_header) == 32, "QRTR v1 header layout");
typedef void (*redwood_qrtr_receive_t)(void *, uint32_t, uint32_t, const void *, size_t);
static struct { uint32_t number; redwood_qrtr_receive_t receive; void *context; } qrtr_ports[16];
static uint32_t qrtr_in, qrtr_peer;
static int qrtr_lock;
static unsigned received, controls, confirms, lock_calls;
static const uint8_t *packet_start, *expected_data;
static size_t packet_length, expected_length;
static uint32_t last_control;

static void lck_mtx_lock(int *lock) { (void)lock; lock_calls++; }
static void lck_mtx_unlock(int *lock) { (void)lock; }
static int quiet_printf(const char *format, ...) { (void)format; return 0; }
static void qrtr_control(const struct qrtr_header *header, const struct qrtr_control *control)
{
    (void)header; controls++; last_control = control->command;
}
static int qrtr_out_packet(uint32_t type, uint32_t from, uint32_t node, uint32_t to, const void *data, uint32_t length)
{
    (void)from; (void)node; (void)to; (void)data;
    assert(type == QRTR_RESUME_TX && length == sizeof(struct qrtr_control));
    confirms++; return 0;
}
static void receive(void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
    assert(context == &received && node == 7 && port == 9);
    assert((const uint8_t *)data >= packet_start && (const uint8_t *)data <= packet_start + packet_length);
    assert(length <= packet_length - ((const uint8_t *)data - packet_start));
    assert(data == expected_data && length == expected_length);
    received++;
}

#define bzero(p, n) memset(p, 0, n)
#define printf quiet_printf
#include "qrtr-input.inc"
#undef printf

static uint8_t *packet(size_t length)
{
    uint8_t *data = calloc(length ? length : 1, 1);
    assert(data != NULL);
    received = controls = confirms = lock_calls = 0;
    qrtr_in = qrtr_peer = 0;
    memset(qrtr_ports, 0, sizeof(qrtr_ports));
    qrtr_ports[0].number = 0x4000;
    qrtr_ports[0].receive = receive;
    qrtr_ports[0].context = &received;
    packet_start = data; packet_length = length;
    return data;
}

static void number(uint8_t *data, uint32_t value)
{
    for (unsigned i = 0; i < 4; i++) data[i] = (uint8_t)(value >> (8 * i));
}

static uint8_t *v1(uint32_t size, size_t available, uint32_t destination)
{
    uint8_t *data = packet(32 + available);
    number(data, 1); number(data + 4, QRTR_DATA);
    number(data + 8, 7); number(data + 12, 9);
    number(data + 20, size); number(data + 24, 1); number(data + 28, destination);
    return data;
}

static uint8_t *compact(uint32_t size, uint8_t options, size_t available, uint16_t destination)
{
    uint8_t *data = packet(16 + available);
    data[0] = 3; data[1] = QRTR_DATA; data[3] = options;
    number(data + 4, size); data[8] = 7; data[10] = 9; data[12] = 1;
    data[14] = (uint8_t)destination; data[15] = (uint8_t)(destination >> 8);
    return data;
}

static void rejected(uint8_t *data)
{
    qrtr_input(data, (uint32_t)packet_length);
    assert(received == 0 && controls == 0 && confirms == 0 && lock_calls == 0);
    assert(qrtr_in == 0 && qrtr_peer == 0);
    free(data);
}

int main(void)
{
    /* Reject before peer registration, response transmission, callback or lock. */
    rejected(v1(UINT32_MAX - 31, 0, QRTR_CONTROL));
    rejected(v1(UINT32_MAX, 1, 0x4000));
    rejected(v1(0, 0, 0x4000));
    rejected(v1(2, 1, 0x4000));
    rejected(v1(REDWOOD_QRTR_MAX + 1, REDWOOD_QRTR_MAX + 1, 0x4000));
    rejected(compact(UINT32_MAX - 15, 0, 0, 0xfffe));
    rejected(compact(UINT32_MAX - 19, 4, 0, 0x4000));
    rejected(compact(1, 255, 254, 0x4000));
    rejected(compact(0, 0, 0, 0x4000));
    rejected(compact(REDWOOD_QRTR_MAX + 1, 0, REDWOOD_QRTR_MAX + 1, 0x4000));
    for (unsigned n = 0; n < 32; n++) {
        uint8_t *data = packet(n); if (n) data[0] = 1; rejected(data);
    }
    for (unsigned n = 0; n < 16; n++) {
        uint8_t *data = packet(n); if (n) data[0] = 3; rejected(data);
    }
    for (unsigned compact_format = 0; compact_format < 2; compact_format++) {
        for (unsigned size = 1; size <= REDWOOD_QRTR_MAX; size += REDWOOD_QRTR_MAX - 1) {
            for (unsigned padding = 0; padding <= 4; padding += 4) {
                uint8_t *data = compact_format ? compact(size, 0, size + padding, 0x4000) : v1(size, size + padding, 0x4000);
                expected_data = data + (compact_format ? 16 : 32); expected_length = size;
                qrtr_input(data, (uint32_t)packet_length);
                assert(received == 1 && qrtr_in == 1 && qrtr_peer == 7);
                free(data);
            }
        }
    }
    /* Options are skipped, and a payload ending exactly at the boundary works. */
    uint8_t *data = compact(1, 255, 256, 0x4000);
    expected_data = data + 271; expected_length = 1;
    data[2] = 1;
    qrtr_input(data, (uint32_t)packet_length);
    assert(received == 1 && confirms == 1);
    free(data);
    /* Control-port translation and zero-filled short controls remain unchanged. */
    data = compact(4, 0, 4, 0xfffe); number(data + 16, QRTR_HELLO);
    qrtr_input(data, (uint32_t)packet_length);
    assert(controls == 1 && last_control == QRTR_HELLO && received == 0);
    free(data);
    puts("QRTR: wrapped/truncated/oversized frames rejected; v1/compact boundary, options, padding and control dispatch passed");
    return 0;
}
