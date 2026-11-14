/* Actual loader parsers, using only synthetic bytes. No firmware or hardware. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../xnuloader/image.h"
#ifdef NDEBUG
#error "This regression test requires assertions (-UNDEBUG)"
#endif

static const uint64_t minimum = 0xfffffff007000000ull;
static const uint64_t limit = 0xfffffff184000000ull;
static void u32(uint8_t *p, uint32_t value)
{
	for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(value >> (i * 8));
}
static void u64(uint8_t *p, uint64_t value)
{
	u32(p, (uint32_t)value); u32(p + 4, (uint32_t)(value >> 32));
}
static void kernel(uint8_t p[768])
{
	memset(p, 0, 768);
	u32(p, MH_MAGIC_64); u32(p + 4, 0x0100000c); u32(p + 12, 2);
	u32(p + 16, 2); u32(p + 20, 72 + 288);
	u32(p + 32, LC_SEGMENT_64); u32(p + 36, 72);
	u64(p + 56, minimum); u64(p + 64, 0x4000);
	u64(p + 72, 512); u64(p + 80, 64); u32(p + 92, 5);
	u32(p + 104, LC_UNIXTHREAD); u32(p + 108, 288);
	u32(p + 112, 6); u32(p + 116, 68); u64(p + 376, minimum + 4);
	memset(p + 512, 0xa5, 64);
}
static int valid_kernel(const uint8_t *p, uint64_t bytes)
{
	struct kernel_layout layout = {0};
	return image_kernel(p, bytes, minimum, limit, &layout);
}
static unsigned add(uint8_t *p, unsigned at, const char *name, const void *body, unsigned bytes)
{
	memset(p + at, 0, 24 + ((bytes + 15) & ~15u));
	memcpy(p + at, name, strlen(name)); u32(p + at + 16, bytes);
	if (bytes) memcpy(p + at + 24, body, bytes);
	return at + 24 + ((bytes + 15) & ~15u);
}
static unsigned payload(uint8_t p[2048])
{
	uint8_t k[768]; kernel(k); memset(p, 0, 2048);
	unsigned at = add(p, 8, "kernel", k, sizeof(k));
	at = add(p, at, "cmdline", "ace5_touch=1", 13);
	at = add(p, at, "checkpoint", "999", 4);
	at += 24; u32(p, PAYLOAD_MAGIC); u32(p + 4, at - 8);
	return at;
}
static void rejects_payload(const uint8_t *p, uint64_t bytes)
{
	struct payload_info info;
	memset(&info, 0xa5, sizeof(info));
	assert(!image_payload(p, bytes, &info));
	assert(!info.kernel && !info.ramdisk && !info.kernel_size && !info.ramdisk_size);
	assert(info.cmdline && !*info.cmdline && !info.checkpoint && !info.forced_base && !info.resident_rd && !info.usb_mode);
}
static void test_payload(void)
{
	uint8_t p[2048], original[2048]; unsigned bytes = payload(p);
	struct payload_info info;
	assert(image_payload(p, bytes, &info) && info.kernel_size == 768 && info.checkpoint == 999);
	assert(valid_kernel(info.kernel, info.kernel_size) && !strcmp(info.cmdline, "ace5_touch=1"));
	memcpy(original, p, sizeof(p));
	for (unsigned n = 0; n < bytes; n++) rejects_payload(p, n);
	rejects_payload(NULL, bytes);
	u32(p + 4, UINT32_MAX); rejects_payload(p, sizeof(p));
	memcpy(p, original, sizeof(p)); u32(p + 24, UINT32_MAX); rejects_payload(p, bytes); /* 16-byte padding overflow */
	memcpy(p, original, sizeof(p)); memset(p + 8, 'x', 16); rejects_payload(p, bytes);
	memcpy(p, original, sizeof(p)); u32(p + 28, 1); rejects_payload(p, bytes); /* reserved */
	memcpy(p, original, sizeof(p)); u32(p + 4, bytes - 8 - 24); rejects_payload(p, bytes); /* no end entry */
	memcpy(p, original, sizeof(p)); p[bytes - 1] = 1; rejects_payload(p, bytes);
	/* The cmdline data has no NUL inside its declared size, even with padded NULs. */
	memcpy(p, original, sizeof(p)); memset(p + 8 + 24 + 768 + 24, 'x', 13); rejects_payload(p, bytes);
	/* Duplicate well-known entries must not silently replace the first one. */
	memset(p, 0, sizeof(p)); unsigned at = add(p, 8, "kernel", "abcd", 4);
	at = add(p, at, "kernel", "efgh", 4); at += 24;
	u32(p, PAYLOAD_MAGIC); u32(p + 4, at - 8); rejects_payload(p, at);
	const char *bad_numbers[] = {"18446744073709551616", "-1", "999junk", ""};
	for (unsigned i = 0; i < sizeof(bad_numbers) / sizeof(bad_numbers[0]); i++) {
		memset(p, 0, sizeof(p)); at = add(p, 8, "checkpoint", bad_numbers[i], (unsigned)strlen(bad_numbers[i]) + 1);
		at += 24; u32(p, PAYLOAD_MAGIC); u32(p + 4, at - 8); rejects_payload(p, at);
	}
	/* Optional entries, unknown binary files and a payload without a kernel remain supported. */
	memset(p, 0, sizeof(p)); at = add(p, 8, "physbase", "ffffffffffffffff", 17);
	at = add(p, at, "rdres", "1000", 5); at = add(p, at, "usb", "1", 2);
	at = add(p, at, "extra", "abcd", 4); at += 24; u32(p, PAYLOAD_MAGIC); u32(p + 4, at - 8);
	assert(image_payload(p, at, &info) && !info.kernel && info.forced_base == UINT64_MAX && info.resident_rd == 4096 && info.usb_mode);
}
static void test_kernel(void)
{
	uint8_t p[768], original[768]; kernel(p); memcpy(original, p, sizeof(p));
	struct kernel_layout layout;
	assert(image_kernel(p, sizeof(p), minimum, limit, &layout));
	assert(layout.vm_lo == minimum && layout.vm_hi == minimum + 0x4000 && layout.entry_va == minimum + 4);
	for (unsigned n = 0; n < 576; n++) assert(!valid_kernel(p, n));
	assert(valid_kernel(p, 576)); /* trailing file padding is not required */
	assert(!valid_kernel(NULL, sizeof(p)));
	const struct { unsigned offset; uint32_t value; } bad32[] = {
		{0, 0}, {4, 0x01000007}, {12, 1}, {16, 0}, {16, UINT32_MAX}, {20, UINT32_MAX},
		{20, 368}, {36, 0}, {36, 7}, {36, 71}, {36, UINT32_MAX}, {96, UINT32_MAX},
		{100, 1}, /* SG_HIGHVM needs a different segment-copy policy. */
		{108, 280}, {108, UINT32_MAX}, {112, 1}, {116, 67}, {92, 1},
	};
	for (unsigned i = 0; i < sizeof(bad32) / sizeof(bad32[0]); i++) {
		memcpy(p, original, sizeof(p)); u32(p + bad32[i].offset, bad32[i].value);
		assert(!valid_kernel(p, sizeof(p)));
	}
	const struct { unsigned offset; uint64_t value; } bad64[] = {
		{56, minimum - 4}, {56, limit}, {64, UINT64_MAX}, {72, UINT64_MAX},
		{80, 0x4001}, {80, 257}, {376, 0}, {376, minimum + 1}, {376, minimum + 64},
	};
	for (unsigned i = 0; i < sizeof(bad64) / sizeof(bad64[0]); i++) {
		memcpy(p, original, sizeof(p)); u64(p + bad64[i].offset, bad64[i].value);
		assert(!valid_kernel(p, sizeof(p)));
	}
	memcpy(p, original, sizeof(p)); u32(p + 16, 3); u32(p + 20, 432);
	memcpy(p + 392, p + 32, 72); assert(!valid_kernel(p, sizeof(p))); /* overlapping segments */
	u64(p + 416, minimum + 0x4000); assert(valid_kernel(p, sizeof(p))); /* disjoint segment */
	u32(p + 392, LC_UNIXTHREAD); u32(p + 396, 72); assert(!valid_kernel(p, sizeof(p)));
	assert(!image_kernel(original, sizeof(original), limit, minimum, &layout));
}
int main(int argc, char **argv)
{
	test_payload(); test_kernel();
	if (argc == 2) {
		FILE *file = fopen(argv[1], "rb"); assert(file);
		uint8_t data[4096]; size_t bytes = fread(data, 1, sizeof(data), file);
		assert(!ferror(file) && feof(file)); fclose(file);
		struct payload_info info; assert(image_payload(data, bytes, &info));
		assert(info.kernel && valid_kernel(info.kernel, info.kernel_size));
	}
	puts("PASS: actual loader XNUP/Mach-O parsers reject truncation, overflow, invalid commands, segments and entry points");
	return 0;
}
