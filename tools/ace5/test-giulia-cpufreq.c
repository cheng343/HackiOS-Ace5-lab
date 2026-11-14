/* Compile the actual EPSS driver against RAM registers and kernel stubs.
 * Cloud-only: this neither touches a device nor establishes hardware safety.
 */
#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uintptr_t vm_offset_t;
typedef size_t vm_size_t;
typedef int boolean_t;
#define FALSE 0
#define NSEC_PER_MSEC 1000000
#define __printflike(a, b) __attribute__((format(printf, a, b)))
enum hackios_platform { HACKIOS_PLATFORM_UNKNOWN, HACKIOS_PLATFORM_REDWOOD,
    HACKIOS_PLATFORM_GIULIA, HACKIOS_PLATFORM_QEMU_VIRT };
typedef struct { unsigned num_cpus; struct { uint32_t phys_id; } cpus[2]; } ml_topology_info_t;

static enum hackios_platform test_platform;
static ml_topology_info_t test_topology;
static uint32_t test_request;
static int test_has_argument, test_map_failed, test_interrupts;
static unsigned test_maps, test_delays;
static uint32_t test_mmio[0x1000 / 4];

static enum hackios_platform hackios_platform(void) { return test_platform; }
static const ml_topology_info_t *ml_get_topology_info(void) { return &test_topology; }
static int PE_parse_boot_argn(const char *name, void *value, unsigned size)
{
    assert(strcmp(name, "ace5_cpu_khz") == 0 && size == sizeof(test_request));
    if (test_has_argument) memcpy(value, &test_request, size);
    return test_has_argument;
}
static int ml_get_interrupts_enabled(void) { return test_interrupts; }
static boolean_t ml_set_interrupts_enabled(boolean_t enabled)
{
    int previous = test_interrupts;
    test_interrupts = enabled;
    return previous;
}
vm_offset_t ml_io_map(vm_offset_t physical, vm_size_t size)
{
    assert(physical == 0x17d91000 && size == sizeof(test_mmio));
    test_maps++;
    return test_map_failed ? 0 : (vm_offset_t)test_mmio;
}
void loader_log_putc(char c) { (void)c; }
static void delay_for_interval(unsigned amount, unsigned units)
{
    assert(amount == 20 && units == NSEC_PER_MSEC);
    test_delays++;
}
static int quiet_printf(const char *format, ...) { (void)format; return 0; }

#define GIULIA_CPUFREQ_HOST_TEST
#define printf(...) quiet_printf(__VA_ARGS__)
#include "../../xnu/files/bsd/dev/giulia_cpufreq.c"
#undef printf

static uint32_t lut_word(unsigned multiple, unsigned cores)
{
    return (1u << 30) | (cores << 16) | multiple;
}

static void reset(void)
{
    giulia_epss_base = 0;
    giulia_epss_offset = giulia_epss_saved = 0;
    giulia_epss_restore_needed = 0;
    test_platform = HACKIOS_PLATFORM_GIULIA;
    test_topology.num_cpus = 1;
    test_topology.cpus[0].phys_id = 0x100;
    test_has_argument = test_interrupts = 1;
    test_map_failed = 0;
    test_maps = test_delays = 0;
    test_request = 1600000;
    memset(test_mmio, 0, sizeof(test_mmio));
    test_mmio[GIULIA_EPSS_ENABLE / 4] = 1;
    test_mmio[GIULIA_EPSS_DCVS_CTRL / 4] = 1;
    const unsigned multiples[] = {19, 35, 50, 70, 80, 94, 118};
    for (unsigned i = 0; i < GIULIA_EPSS_ROWS; i++)
        test_mmio[GIULIA_EPSS_FREQ_LUT / 4 + i] =
            lut_word(multiples[i < 7 ? i : 6], 2);
    test_mmio[GIULIA_EPSS_PERF_STATE / 4] = 1;
    test_mmio[GIULIA_EPSS_PERF_STATE / 4 + 1] = 2;
    test_mmio[GIULIA_EPSS_DOMAIN_STATE / 4] = 35;
}

static void no_vote(int expect_maps)
{
    uint32_t before[sizeof(test_mmio) / sizeof(test_mmio[0])];
    memcpy(before, test_mmio, sizeof(before));
    giulia_cpufreq_init();
    giulia_cpufreq_quiesce();
    assert(memcmp(before, test_mmio, sizeof(before)) == 0);
    assert(!giulia_epss_restore_needed && test_maps == (unsigned)expect_maps);
}

int main(void)
{
    uint32_t domain, slot, row, khz;
    const unsigned domains[] = {0, 0, 3, 3, 3, 1, 1, 2};
    const unsigned slots[] = {0, 1, 0, 1, 2, 0, 1, 0};
    for (unsigned cpu = 0; cpu < 8; cpu++) {
        assert(giulia_epss_domain_slot(cpu << 8, &domain, &slot));
        assert(domain == domains[cpu] && slot == slots[cpu]);
    }
    assert(!giulia_epss_domain_slot(1, &domain, &slot));
    assert(!giulia_epss_domain_slot(0x800, &domain, &slot));
    assert(!giulia_epss_domain_slot(0x10000, &domain, &slot));
    assert(!giulia_epss_domain_slot(0, NULL, &slot));

    for (unsigned platform = 0; platform < 4; platform++) {
        if (platform == HACKIOS_PLATFORM_GIULIA) continue;
        reset(); test_platform = (enum hackios_platform)platform; no_vote(0);
    }
    reset(); test_has_argument = 0; no_vote(0);
    const uint32_t rejected[] = {0, 599999, 1804801, UINT32_MAX};
    for (unsigned i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        reset(); test_request = rejected[i]; no_vote(0);
    }
    reset(); test_topology.num_cpus = 2; no_vote(0);
    reset(); test_topology.num_cpus = 0; no_vote(0);
    for (unsigned cpu = 2; cpu < 8; cpu++) {
        reset(); test_topology.cpus[0].phys_id = cpu << 8; no_vote(0);
    }
    reset(); test_topology.cpus[0].phys_id = 0x10000; no_vote(0);
    reset(); test_map_failed = 1; no_vote(1);
    reset(); test_mmio[GIULIA_EPSS_ENABLE / 4] = 0; no_vote(1);
    reset(); test_mmio[GIULIA_EPSS_PERF_STATE / 4 + 1] = 40; no_vote(1);
    reset(); test_mmio[GIULIA_EPSS_PERF_STATE / 4 + 1] = 4; no_vote(1); /* Already at our row. */
    reset(); memset(test_mmio + GIULIA_EPSS_FREQ_LUT / 4, 0, GIULIA_EPSS_ROWS * 4); no_vote(1);
    reset(); test_mmio[GIULIA_EPSS_FREQ_LUT / 4 + 3] = lut_word(20, 2); no_vote(1);
    reset();
    for (unsigned i = 0; i < GIULIA_EPSS_ROWS; i++)
        test_mmio[GIULIA_EPSS_FREQ_LUT / 4 + i] = lut_word(118, 2);
    no_vote(1); /* The lowest hardware frequency exceeds the explicit cap. */

    reset();
    uint32_t *lut = test_mmio + GIULIA_EPSS_FREQ_LUT / 4;
    assert(!giulia_epss_choose_row(NULL, 1000000, &row, &khz));
    assert(giulia_epss_choose_row(lut, 600000, &row, &khz) && row == 0 && khz == 364800);
    assert(giulia_epss_choose_row(lut, 1804800, &row, &khz) && row == 5 && khz == 1804800);
    lut[5] = lut_word(94, 1); /* Reduced active-core count is a boost row. */
    assert(giulia_epss_choose_row(lut, 1804800, &row, &khz) && row == 4 && khz == 1536000);
    lut[0] = (2u << 16) | 255; /* Alternate source L value is never an XO request. */
    assert(giulia_epss_choose_row(lut, 1000000, &row, &khz) && row == 2 && khz == 960000);

    /* Check both A520s in shared/per-core mode, and restore precisely the
     * changed slot. In particular CPU1 must not overwrite CPU0's per-core vote. */
    for (unsigned core = 0; core < 2; core++) {
        for (unsigned per_core = 0; per_core < 2; per_core++) {
            reset();
            uint32_t before[sizeof(test_mmio) / sizeof(test_mmio[0])];
            test_topology.cpus[0].phys_id = core << 8;
            test_mmio[GIULIA_EPSS_DCVS_CTRL / 4] = per_core;
            memcpy(before, test_mmio, sizeof(before));
            unsigned voted = GIULIA_EPSS_PERF_STATE / 4 + (per_core ? core : 0);
            giulia_cpufreq_init();
            assert(test_maps == 1 && test_delays == 1 && test_interrupts == 1);
            assert(giulia_epss_restore_needed && test_mmio[voted] == 4);
            for (unsigned i = 0; i < sizeof(test_mmio) / sizeof(test_mmio[0]); i++)
                if (i != voted) assert(test_mmio[i] == before[i]);
            giulia_cpufreq_init();
            assert(test_maps == 1); /* The saved original cannot be overwritten. */
            test_interrupts = 0; /* As on the panic/loader-return path. */
            giulia_cpufreq_quiesce();
            assert(!giulia_epss_restore_needed && test_interrupts == 0);
            assert(memcmp(before, test_mmio, sizeof(before)) == 0);
            giulia_cpufreq_quiesce();
            assert(memcmp(before, test_mmio, sizeof(before)) == 0);
        }
    }
    puts("EPSS: actual driver opt-in/platform/topology guards, bounded LUT selection, per-core vote and exact restore passed (RAM MMIO only)");
    return 0;
}
