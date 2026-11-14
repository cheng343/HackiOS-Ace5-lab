/* Real a750_probe admission prefix with boot arguments and DT stubs.
 * Extraction ends before the first MMIO mapping: this tests decisions only,
 * not device behavior. Run through test-gpu-lifecycle.py in cloud CI. */
#ifdef NDEBUG
#error "Compile this test with -UNDEBUG"
#endif
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef void *DTEntry;
#define kSuccess 0
static uint32_t stage, direct, bootcpu, seconds, step_delay;
static uint32_t a750_gpu_direct, a750_gpu_keep, probe_pause;
static bool hardware_admitted, board_present = true;
static uint64_t gic_reg[4] = {0x17100000, 0x10000, 0x17180000, 0x100000};
static uint64_t pe_arm_get_soc_base_phys(void) { return 0; }
static int SecureDTLookupEntry(DTEntry parent, const char *path, DTEntry *entry)
{
    (void)parent;
    assert(!strcmp(path, "/arm-io/gic"));
    *entry = gic_reg;
    return board_present ? kSuccess : -1;
}
static int SecureDTGetProperty(DTEntry entry, const char *name, const void **value, unsigned *size)
{
    assert(entry == gic_reg && !strcmp(name, "reg"));
    *value = gic_reg;
    *size = sizeof(gic_reg);
    return kSuccess;
}
static bool PE_parse_boot_argn(const char *name, void *value, unsigned size)
{
    const struct { const char *name; uint32_t value; } args[] = {
        {"ace5_gpu_probe", stage}, {"ace5_gpu_direct", direct},
        {"bootcpu", bootcpu}, {"loader_return_after", seconds},
        {"ace5_gpu_step_delay", step_delay}, {"ace5_gpu_pause", 0},
        {"adreno", 0}, {"gpu", 0}
    };
    assert(size == sizeof(uint32_t));
    for (unsigned i = 0; i < sizeof(args) / sizeof(args[0]); i++) {
        if (!strcmp(name, args[i].name)) {
            memcpy(value, &args[i].value, size);
            return true;
        }
    }
    assert(!"unexpected boot argument");
    return false;
}
static int a750_log(const char *format, ...) { (void)format; return 0; }
#include "gpu-probe-admission.inc"

static void check(uint32_t st, uint32_t di, uint32_t keep, uint32_t cpu,
                  uint32_t sec, uint32_t trace, bool expected)
{
    stage = st; direct = di; a750_gpu_keep = keep; bootcpu = cpu;
    seconds = sec; step_delay = trace; a750_gpu_direct = 0; probe_pause = 0;
    hardware_admitted = false;
    (void)a750_probe();
    assert(hardware_admitted == expected);
}
int main(void)
{
    /* Regression: the exact no-timer launch previously passed phone.py but
     * returned here before touching the GPU. Every incomplete profile stays off. */
    check(20, 1, 1, 1, 0, 0, true);
    check(20, 1, 1, 0, 0, 0, false);
    check(20, 1, 1, 2, 0, 0, false);
    check(20, 0, 1, 1, 0, 0, false);
    check(20, 1, 0, 1, 0, 0, false);
    check(20, 1, 1, 1, 0, 1, false);
    check(19, 1, 1, 1, 0, 0, false);
    check(19, 1, 0, 1, 0, 0, false);
    check(21, 1, 1, 1, 0, 0, false);
    check(0, 1, 1, 1, 0, 0, false);
    /* Finite power probes retain their CPU and minimum-cleanup budget gates. */
    check(2, 0, 0, 0, 1, 0, true);
    check(3, 0, 0, 0, 1, 0, false);
    check(3, 0, 0, 1, 1, 0, true);
    check(3, 0, 0, 1, 0, 0, false);
    check(17, 0, 0, 1, 9, 0, false);
    check(17, 0, 0, 1, 10, 0, true);
    check(17, 0, 0, 1, 10, 1, false);
    check(17, 0, 0, 1, 11, 1, true);
    check(17, 0, 0, 1, 729, 3000, false);
    check(17, 0, 0, 1, 730, 3000, true);
    check(17, 0, 0, 1, 3600, 3001, false);
    check(17, 0, 0, 1, 3600, UINT32_MAX, false);
    check(20, 1, 1, 1, 10, 0, true);
    check(20, 1, 1, 1, 120, 1, false);
    /* A valid no-timer profile never bypasses board identity checks. */
    board_present = false;
    check(20, 1, 1, 1, 0, 0, false);
    board_present = true; gic_reg[0] = 0x17000000;
    check(20, 1, 1, 1, 0, 0, false);
    gic_reg[0] = 0x17100000; gic_reg[2] = 0x17170000;
    check(20, 1, 1, 1, 0, 0, false);
    puts("A750 probe admission tests passed");
    return 0;
}
