#ifdef NDEBUG
#error "Loader tests require assertions: use -UNDEBUG"
#endif
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "constants.inc"

static uint8_t payload_memory[1024], resident_memory[1024], rx_memory[USB_CHUNK];
#define PAYLOAD_BASE ((uintptr_t)payload_memory)
static uint8_t *usb_rx_buf = rx_memory;
static uint64_t win_base, win_size, resident_rd_size;
static int download_to_ramdisk;
static unsigned log_len;
struct payload_info { int unused; };
static int parse_payload(struct payload_info *pi, int verbose) { (void)pi; (void)verbose; return 1; }
static int choose_window(const struct payload_info *pi) { (void)pi; return 1; }
static int starts_with(const char *s, const char *prefix) { return strncmp(s, prefix, strlen(prefix)) == 0; }
static uint64_t parse_number(const char *s, unsigned base) { return strtoull(s, NULL, (int)base); }
static char reply_code[5], reply_text[64];
static uint64_t data_reply;
static void fb_reply(const char *code, const char *text)
{
    snprintf(reply_code, sizeof(reply_code), "%s", code);
    snprintf(reply_text, sizeof(reply_text), "%s", text);
}
static void fb_reply_data(uint64_t total) { data_reply = total; }
static int64_t received[8];
static unsigned received_count, received_index;
static int64_t usb_rx(uint8_t *dst, uint64_t want)
{
    assert(received_index < received_count && want <= sizeof(rx_memory));
    int64_t result = received[received_index++];
    if (result > 0) memset(dst, 0x5a, (size_t)((uint64_t)result > want ? want : (uint64_t)result));
    return result;
}
#include "commands.inc"

static void reset(void)
{
    memset(payload_memory, 0xcc, sizeof(payload_memory));
    memset(resident_memory, 0xdd, sizeof(resident_memory));
    memset(reply_code, 0, sizeof(reply_code));
    memset(reply_text, 0, sizeof(reply_text));
    resident_rd_size = 17; payload_available = 64; download_to_ramdisk = 0;
    win_base = (uintptr_t)resident_memory - RESIDENT_RD_OFFSET;
    win_size = RESIDENT_RD_OFFSET + RESIDENT_RD_RESERVE + sizeof(resident_memory);
    received_count = received_index = 0; data_reply = 0;
}
static void receive(int64_t first, int64_t second)
{
    received[0] = first; received[1] = second; received_count = 2;
}
static void filled(const uint8_t *p, unsigned count, uint8_t value)
{
    for (unsigned i = 0; i < count; i++) assert(p[i] == value);
}
int main(void)
{
    reset(); command("oem ramdisk"); assert(download_to_ramdisk == 1);
    command("oem ramdisk none"); assert(download_to_ramdisk == 0 && resident_rd_size == 0);
    receive(32, -1); command("download:00000020");
    assert(payload_available == 32 && resident_rd_size == 0 && !strcmp(reply_code, "OKAY"));
    filled(payload_memory, 32, 0x5a); filled(resident_memory, sizeof(resident_memory), 0xdd);

    reset(); receive(16, -1); command("download:00000020");
    assert(payload_available == 0 && !strcmp(reply_code, "FAIL"));
    assert(!strcmp(reply_text, "short download")); filled(payload_memory + 16, 16, 0xcc);
    receive(16, 16); received_index = 0; command("download:00000020");
    assert(payload_available == 32 && !strcmp(reply_code, "OKAY") && data_reply == 32);
    filled(payload_memory, 32, 0x5a); assert(payload_memory[32] == 0xcc);

    reset(); receive(-1, -1); command("download:00000020");
    assert(payload_available == 0 && !strcmp(reply_code, "FAIL")); filled(payload_memory, 32, 0xcc);
    reset(); receive(0, -1); command("download:00000020");
    assert(payload_available == 0 && !strcmp(reply_code, "FAIL"));
    reset(); receive(33, -1); command("download:00000020");
    assert(payload_available == 0 && !strcmp(reply_code, "FAIL")); filled(payload_memory, 33, 0xcc);

    reset(); command("download:00000000");
    assert(payload_available == 0 && !strcmp(reply_code, "FAIL") && received_index == 0);
    reset(); command("download:04000000");
    assert(payload_available == 0 && !strcmp(reply_code, "FAIL") && received_index == 0);

    reset(); command("oem ramdisk"); receive(16, -1); command("download:00000020");
    assert(resident_rd_size == 0 && payload_available == 64 && !strcmp(reply_code, "FAIL"));
    filled(payload_memory, 32, 0xcc);
    reset(); command("oem ramdisk"); receive(32, -1); command("download:00000020");
    assert(resident_rd_size == 32 && payload_available == 64 && !strcmp(reply_code, "OKAY"));
    filled(resident_memory, 32, 0x5a); filled(payload_memory, 32, 0xcc);
    puts("PASS: actual USB branches cancel ramdisk mode, reject short/oversized completions and publish only complete payloads");
    return 0;
}
