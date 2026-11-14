/*
 * Board identity supplied by xnuloader in /arm-io/device_type.
 * The iPhone identity at the root is shared by all targets, and redwood and
 * giulia both use 0x00400000 as their SoC base: neither identifies a board.
 */
#ifndef _SYS_HACKIOS_PLATFORM_H_
#define _SYS_HACKIOS_PLATFORM_H_

#include <libkern/libkern.h>
#include <pexpert/device_tree.h>

enum hackios_platform {
	HACKIOS_PLATFORM_UNKNOWN,
	HACKIOS_PLATFORM_REDWOOD,
	HACKIOS_PLATFORM_GIULIA,
	HACKIOS_PLATFORM_QEMU_VIRT
};

static inline enum hackios_platform
hackios_platform(void)
{
	DTEntry entry;
	const void *prop = NULL;
	unsigned int size = 0;

	if (SecureDTLookupEntry(NULL, "/arm-io", &entry) != kSuccess ||
	    SecureDTGetProperty(entry, "device_type", &prop, &size) != kSuccess || prop == NULL) {
		return HACKIOS_PLATFORM_UNKNOWN;
	}
	/* dt_prop_str includes the NUL. Reject truncation, prefixes and extra data. */
	if (size == sizeof("redwood") && memcmp(prop, "redwood", sizeof("redwood")) == 0) {
		return HACKIOS_PLATFORM_REDWOOD;
	}
	if (size == sizeof("giulia") && memcmp(prop, "giulia", sizeof("giulia")) == 0) {
		return HACKIOS_PLATFORM_GIULIA;
	}
	if (size == sizeof("qemu-virt") && memcmp(prop, "qemu-virt", sizeof("qemu-virt")) == 0) {
		return HACKIOS_PLATFORM_QEMU_VIRT;
	}
	return HACKIOS_PLATFORM_UNKNOWN;
}

#endif /* _SYS_HACKIOS_PLATFORM_H_ */
