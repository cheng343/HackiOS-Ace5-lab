/*
 * SM8650 EPSS table decoding, independent of the kernel/MMIO implementation.
 * Register fields and domain numbering are documented by the public sources
 * listed in docs/ACE5-EPSS.md. This is an original, limited XNU implementation.
 */
#ifndef _SYS_GIULIA_CPUFREQ_H_
#define _SYS_GIULIA_CPUFREQ_H_

#include <stdint.h>

#define GIULIA_EPSS_PHYS          0x17d91000UL
#define GIULIA_EPSS_SIZE          0x1000UL
#define GIULIA_EPSS_ENABLE        0x000u
#define GIULIA_EPSS_DOMAIN_STATE  0x020u
#define GIULIA_EPSS_DCVS_CTRL      0x0b0u
#define GIULIA_EPSS_FREQ_LUT       0x100u
#define GIULIA_EPSS_PERF_STATE     0x320u
#define GIULIA_EPSS_ROWS           40u
#define GIULIA_EPSS_XO_KHZ         19200u
#define GIULIA_CPU_MIN_REQUEST     600000u
#define GIULIA_CPU_MAX_REQUEST     1804800u

/* The domain numbers are hardware indices, not Linux policy numbers. */
static inline int
giulia_epss_domain_slot(uint32_t phys_id, uint32_t *domain, uint32_t *slot)
{
	static const uint8_t domains[8] = { 0, 0, 3, 3, 3, 1, 1, 2 };
	static const uint8_t slots[8] = { 0, 1, 0, 1, 2, 0, 1, 0 };
	if (domain == 0 || slot == 0 || (phys_id & ~0x700u) != 0) {
		return 0;
	}
	*domain = domains[phys_id >> 8];
	*slot = slots[phys_id >> 8];
	return 1;
}

static inline int
giulia_epss_request_valid(uint32_t khz)
{
	return khz >= GIULIA_CPU_MIN_REQUEST && khz <= GIULIA_CPU_MAX_REQUEST;
}

/*
 * Pick an actual firmware-programmed row at or below the explicit request.
 * The Ace 5 vendor EPSS driver has turbo_ind_support=false: core_count==1
 * alone is NOT a turbo marker. A drop below the first row's core count is a
 * boost row and is excluded here. Repeated frequency marks the table's end.
 * Source 0 is the alternate clock, which this limited driver never selects;
 * no guessed alternate-clock frequency is used as a performance request.
 */
static inline int
giulia_epss_choose_row(const uint32_t *lut, uint32_t request_khz,
    uint32_t *row_out, uint32_t *khz_out)
{
	uint32_t previous = 0, first_count, selected_row = 0, selected_khz = 0;
	if (lut == 0 || row_out == 0 || khz_out == 0 || !giulia_epss_request_valid(request_khz)) {
		return 0;
	}
	first_count = (lut[0] >> 16) & 7u;
	for (uint32_t row = 0; row < GIULIA_EPSS_ROWS; row++) {
		uint32_t entry = lut[row], source = entry >> 30;
		uint32_t khz = GIULIA_EPSS_XO_KHZ * (entry & 0xffu);
		uint32_t count = (entry >> 16) & 7u;
		/* Do not interpret the alternate source's L value as an XO multiple. */
		if (source == 0) {
			if (row != 0) {
				return 0; /* An unexpected clock-source change: leave the vote alone. */
			}
			continue;
		}
		if (khz == 0 || (previous != 0 && khz < previous)) {
			return 0;
		}
		if (previous != 0 && khz == previous) {
			break;
		}
		previous = khz;
		if (count >= first_count && khz <= request_khz) {
			selected_row = row;
			selected_khz = khz;
		}
	}
	if (selected_khz == 0) {
		return 0;
	}
	*row_out = selected_row;
	*khz_out = selected_khz;
	return 1;
}

void giulia_cpufreq_init(void);
void giulia_cpufreq_quiesce(void);

#endif
