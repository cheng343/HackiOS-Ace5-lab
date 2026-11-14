/* Bounded readers for the local XNUP payload and an ARM64 Mach-O kernel.
 * These only validate bytes; they do not allocate memory or start hardware.
 */
#ifndef XNULOADER_IMAGE_H
#define XNULOADER_IMAGE_H
#include <stdint.h>
#include <stddef.h>

#define PAYLOAD_MAGIC 0x50554e58u
struct payload_header { uint32_t magic, size; };
struct payload_entry { char name[16]; uint32_t size, reserved; };
struct payload_info {
	const uint8_t *kernel, *ramdisk;
	uint32_t kernel_size, ramdisk_size;
	const char *cmdline;
	uint64_t checkpoint, forced_base, resident_rd;
	int usb_mode;
};

#define MH_MAGIC_64 0xfeedfacfu
#define LC_SEGMENT_64 0x19
#define LC_UNIXTHREAD 0x5
struct mach_header_64 { uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved; };
struct load_command { uint32_t cmd, cmdsize; };
struct segment_command_64 {
	uint32_t cmd, cmdsize; char segname[16];
	uint64_t vmaddr, vmsize, fileoff, filesize;
	uint32_t maxprot, initprot, nsects, flags;
};
struct kernel_layout { uint64_t vm_lo, vm_hi, entry_va; };

static uint32_t image_u32(const uint8_t *p)
{
	return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t image_u64(const uint8_t *p)
{
	return image_u32(p) | (uint64_t)image_u32(p + 4) << 32;
}
static int image_string(const uint8_t *p, uint64_t size)
{
	for (uint64_t i = 0; i < size; i++) if (!p[i]) return 1;
	return 0;
}
static int image_name(const uint8_t *p, const char *name)
{
	for (unsigned i = 0; i < 16; i++) {
		if (p[i] != (uint8_t)name[i]) return 0;
		if (!p[i]) return 1;
	}
	return 0;
}
static int image_number(const uint8_t *p, unsigned base, uint64_t *out)
{
	uint64_t value = 0;
	if (!*p) return 0;
	for (; *p; p++) {
		unsigned digit = *p >= '0' && *p <= '9' ? *p - '0' :
		    *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 : 16;
		if (digit >= base || value > (UINT64_MAX - digit) / base) return 0;
		value = value * base + digit;
	}
	*out = value;
	return 1;
}
static int image_payload(const uint8_t *data, uint64_t available, struct payload_info *out)
{
	struct payload_info pi = {0};
	pi.cmdline = "";
	*out = pi; /* Failed parses never expose partially validated pointers. */
	if (!data || available < 8 || image_u32(data) != PAYLOAD_MAGIC) return 0;
	uint64_t size = image_u32(data + 4), offset = 8;
	if (size < 24 || size > available - 8) return 0;
	size += 8;
	unsigned seen = 0;
	while (size - offset >= 24) {
		const uint8_t *entry = data + offset, *body = entry + 24;
		uint32_t bytes = image_u32(entry + 16);
		if (!image_string(entry, 16) || image_u32(entry + 20)) return 0;
		if (!entry[0]) {
			for (unsigned i = 0; i < 24; i++) if (entry[i]) return 0;
			if (offset + 24 != size) return 0;
			*out = pi;
			return 1;
		}
		uint64_t padded = ((uint64_t)bytes + 15) & ~15ull;
		if (padded > size - offset - 24) return 0;
		unsigned kind = image_name(entry, "kernel") ? 1 : image_name(entry, "ramdisk") ? 2 :
		    image_name(entry, "cmdline") ? 4 : image_name(entry, "physbase") ? 8 :
		    image_name(entry, "rdres") ? 16 : image_name(entry, "checkpoint") ? 32 :
		    image_name(entry, "usb") ? 64 : 0;
		if (kind && (seen & kind)) return 0;
		seen |= kind;
		if (kind >= 4 && !image_string(body, bytes)) return 0;
		if (kind == 1) { pi.kernel = body; pi.kernel_size = bytes; }
		else if (kind == 2) { pi.ramdisk = body; pi.ramdisk_size = bytes; }
		else if (kind == 4) pi.cmdline = (const char *)body;
		else if (kind == 8 && !image_number(body, 16, &pi.forced_base)) return 0;
		else if (kind == 16 && !image_number(body, 16, &pi.resident_rd)) return 0;
		else if (kind == 32 && !image_number(body, 10, &pi.checkpoint)) return 0;
		else if (kind == 64) pi.usb_mode = 1;
		offset += 24 + padded;
	}
	return 0; /* Missing end entry, including truncated padding. */
}

static int image_kernel(const uint8_t *data, uint64_t size, uint64_t minimum,
    uint64_t limit, struct kernel_layout *out)
{
	struct kernel_layout layout = {UINT64_MAX, 0, 0};
	uint64_t begin[128], end[128];
	unsigned segments = 0, have_thread = 0, executable_entry = 0;
	if (!data || size < 32 || image_u32(data) != MH_MAGIC_64 ||
	    image_u32(data + 4) != 0x0100000c || image_u32(data + 12) != 2 || minimum >= limit) return 0;
	uint32_t count = image_u32(data + 16), commands = image_u32(data + 20);
	if (!count || commands > size - 32 || count > commands / 8) return 0;
	uint64_t offset = 32, stop = 32ull + commands;
	for (uint32_t i = 0; i < count; i++) {
		if (stop - offset < 8) return 0;
		const uint8_t *p = data + offset;
		uint32_t cmd = image_u32(p), bytes = image_u32(p + 4);
		if (bytes < 8 || (bytes & 7) || bytes > stop - offset) return 0;
		if (cmd == LC_SEGMENT_64) {
			if (bytes < 72 || (uint64_t)image_u32(p + 64) * 80 > bytes - 72) return 0;
			/* SG_HIGHVM places file bytes at the high end; our copier loads low. */
			if (image_u32(p + 68) & 1) return 0;
			uint64_t va = image_u64(p + 24), mem = image_u64(p + 32);
			uint64_t file = image_u64(p + 40), length = image_u64(p + 48);
			if (length > mem || file > size || length > size - file) return 0;
			if (mem) {
				if (va < minimum || va >= limit || mem > limit - va || segments == 128) return 0;
				for (unsigned j = 0; j < segments; j++)
					if (va < end[j] && begin[j] < va + mem) return 0;
				begin[segments] = va; end[segments++] = va + mem;
				if (va < layout.vm_lo) layout.vm_lo = va;
				if (va + mem > layout.vm_hi) layout.vm_hi = va + mem;
			}
		} else if (cmd == LC_UNIXTHREAD) {
			/* ARM_THREAD_STATE64 (6), 68 32-bit words; PC is 64-bit register 32. */
			if (have_thread++ || bytes < 288 || image_u32(p + 8) != 6 || image_u32(p + 12) != 68) return 0;
			layout.entry_va = image_u64(p + 16 + 32 * 8);
		}
		offset += bytes;
	}
	if (offset != stop || !segments || !have_thread || (layout.entry_va & 3)) return 0;
	/* The entry must be copied executable code, rather than a hole or zero-fill. */
	for (offset = 32; offset < stop; offset += image_u32(data + offset + 4)) {
		const uint8_t *p = data + offset;
		if (image_u32(p) == LC_SEGMENT_64 && (image_u32(p + 60) & 4) &&
		    layout.entry_va >= image_u64(p + 24) &&
		    layout.entry_va - image_u64(p + 24) < image_u64(p + 48)) executable_entry = 1;
	}
	if (!executable_entry) return 0;
	*out = layout;
	return 1;
}
#endif
