/*
 * OnePlus Ace 5 / SM8650: opt-in EPSS performance vote for one A520 core.
 * ace5_cpu_khz=0 (default) does no MMIO. An explicit 600000..1804800 kHz
 * request selects a non-boost firmware LUT row, rounded down. This is a
 * bounded fixed vote, not a load governor or an implementation of Android's
 * software thermal policy. Hardware LMh/voltage controls are left untouched.
 *
 * The original vote is restored by loader_return after the other CPUs stop.
 * No DDR, RPMh, clock-enable, voltage, interrupt or thermal register is written.
 * Public register/protocol references and scope: docs/ACE5-EPSS.md.
 */
#ifndef GIULIA_CPUFREQ_HOST_TEST
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/hackios_platform.h>
#include <sys/systm.h>
#endif
#include "../sys/giulia_cpufreq.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);

static vm_offset_t giulia_epss_base;
static uint32_t giulia_epss_offset, giulia_epss_saved;
static volatile int giulia_epss_restore_needed;

static void
giulia_cpufreq_log(const char *format, ...) __printflike(1, 2);

static void
giulia_cpufreq_log(const char *format, ...)
{
	char line[256];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	for (const char *p = line; *p; p++) {
		loader_log_putc(*p);
	}
	/* loader_return may run with interrupts disabled, where printf is unsafe. */
	if (ml_get_interrupts_enabled()) {
		printf("%s", line);
	}
}

void
giulia_cpufreq_quiesce(void)
{
	boolean_t interrupts;
	uint32_t readback;
	if (!giulia_epss_restore_needed) {
		return;
	}
	/* This driver admits only one XNU CPU and never starts a worker thread. */
	interrupts = ml_set_interrupts_enabled(FALSE);
	*(volatile uint32_t *)(giulia_epss_base + giulia_epss_offset) = giulia_epss_saved;
	__sync_synchronize();
	readback = *(volatile uint32_t *)(giulia_epss_base + giulia_epss_offset);
	if (readback == giulia_epss_saved) {
		giulia_epss_restore_needed = 0;
	}
	ml_set_interrupts_enabled(interrupts);
	giulia_cpufreq_log("ace5-cpufreq: restore slot %#x to row %u: %s (read %u)\n",
	    giulia_epss_offset, giulia_epss_saved,
	    readback == giulia_epss_saved ? "done" : "FAILED", readback);
}

void
giulia_cpufreq_init(void)
{
	const ml_topology_info_t *topology;
	uint32_t requested = 0, domain, slot, row, khz, old_vote, readback;
	uint32_t lut[GIULIA_EPSS_ROWS];
	boolean_t interrupts;
	if (hackios_platform() != HACKIOS_PLATFORM_GIULIA ||
	    !PE_parse_boot_argn("ace5_cpu_khz", &requested, sizeof(requested)) || requested == 0) {
		return;
	}
	if (!giulia_epss_request_valid(requested)) {
		giulia_cpufreq_log("ace5-cpufreq: request %u kHz rejected; allowed %u..%u\n",
		    requested, GIULIA_CPU_MIN_REQUEST, GIULIA_CPU_MAX_REQUEST);
		return;
	}
	topology = ml_get_topology_info();
	if (topology == NULL || topology->num_cpus != 1 ||
	    !giulia_epss_domain_slot(topology->cpus[0].phys_id, &domain, &slot) || domain != 0) {
		giulia_cpufreq_log("ace5-cpufreq: only one A520 XNU CPU is supported; no vote\n");
		return;
	}
	if (giulia_epss_restore_needed) {
		return; /* Never replace the saved loader vote with one of our own. */
	}
	giulia_epss_base = ml_io_map(GIULIA_EPSS_PHYS, GIULIA_EPSS_SIZE);
	if (giulia_epss_base == 0 ||
	    !(*(volatile uint32_t *)(giulia_epss_base + GIULIA_EPSS_ENABLE) & 1u)) {
		giulia_cpufreq_log("ace5-cpufreq: domain 0 is unavailable/disabled; no vote\n");
		return;
	}
	for (uint32_t i = 0; i < GIULIA_EPSS_ROWS; i++) {
		lut[i] = *(volatile uint32_t *)(giulia_epss_base + GIULIA_EPSS_FREQ_LUT + 4u * i);
	}
	if (!giulia_epss_choose_row(lut, requested, &row, &khz)) {
		giulia_cpufreq_log("ace5-cpufreq: no valid non-boost LUT row <= %u kHz; no vote\n", requested);
		return;
	}
	if (!(*(volatile uint32_t *)(giulia_epss_base + GIULIA_EPSS_DCVS_CTRL) & 1u)) {
		slot = 0; /* Shared domain request; otherwise write only our core's slot. */
	}
	giulia_epss_offset = GIULIA_EPSS_PERF_STATE + 4u * slot;
	old_vote = *(volatile uint32_t *)(giulia_epss_base + giulia_epss_offset);
	if (old_vote >= GIULIA_EPSS_ROWS) {
		giulia_cpufreq_log("ace5-cpufreq: unexpected original vote %#x; left untouched\n", old_vote);
		return;
	}
	giulia_epss_saved = old_vote;
	if (old_vote != row) {
		interrupts = ml_set_interrupts_enabled(FALSE);
		/* Publish the restore record before making the reversible hardware vote. */
		giulia_epss_restore_needed = 1;
		__sync_synchronize();
		*(volatile uint32_t *)(giulia_epss_base + giulia_epss_offset) = row;
		__sync_synchronize();
		readback = *(volatile uint32_t *)(giulia_epss_base + giulia_epss_offset);
		ml_set_interrupts_enabled(interrupts);
		if (readback != row) {
			giulia_cpufreq_log("ace5-cpufreq: vote readback mismatch (%u != %u); restoring\n", readback, row);
			giulia_cpufreq_quiesce();
			return;
		}
	}
	delay_for_interval(20, NSEC_PER_MSEC);
	readback = *(volatile uint32_t *)(giulia_epss_base + GIULIA_EPSS_DOMAIN_STATE) & 0xffu;
	giulia_cpufreq_log("ace5-cpufreq: A520 domain 0 slot %u row %u -> %u, requested %u kHz, LUT %u kHz, hardware %u kHz; LMh unchanged\n",
	    slot, old_vote, row, requested, khz, readback * GIULIA_EPSS_XO_KHZ);
}
