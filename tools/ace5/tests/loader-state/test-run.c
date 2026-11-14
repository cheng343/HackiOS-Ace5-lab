#ifdef NDEBUG
#error "Loader tests require assertions: use -UNDEBUG"
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define HACKIOS_STORAGE_WRITES 0
static int gamut_p3, block_enabled, block_probe_requested, hc_requested;
static int block_write_requested, block_write_part;
static int hc_known = 1; /* Persistent recovery information must survive argument reset. */
static unsigned surveys, block_starts, charger_starts, hc_starts;
static const struct { unsigned core_shift; } platform = {8};
#define PLAT (&platform)
static int starts_with(const char *s, const char *prefix) { return strncmp(s, prefix, strlen(prefix)) == 0; }
static void charger_start(void) { charger_starts++; }
static void hc_init(void) { hc_starts++; }
static void ufs_survey(void) { surveys++; }
static void block_start(void) { block_starts++; block_enabled = 1; }
static int run(const char *cmdline)
{
    const struct { const char *cmdline; } pi = {cmdline};
    uint64_t kernel_mpidr = 0;
    int more_cores = 0, battery_requested = 0, ufs_requested = 0;
    int torch_requested = 0, camera_requested = 0;
#include "run-state.inc"
    (void)kernel_mpidr; (void)more_cores; (void)torch_requested; (void)camera_requested;
    return ufs_requested;
}
int main(void)
{
    assert(run("ufs=2 ufs_probe=1 ufs_native=1 ufs_write=1 gamut=p3") == 2);
    assert(block_enabled && block_probe_requested && hc_requested && gamut_p3);
    assert(block_starts == 1 && surveys == 1 && hc_starts == 1);
    assert(!block_write_requested && block_write_part == -1 && hc_known == 1);
    assert(run("") == 0);
    assert(!block_enabled && !block_probe_requested && !hc_requested && !gamut_p3);
    assert(block_starts == 1 && surveys == 1 && hc_starts == 1 && hc_known == 1);
    assert(run("ufs=2") == 2);
    assert(block_enabled && !block_probe_requested && !hc_requested);
    assert(block_starts == 2 && surveys == 2 && hc_known == 1);
    assert(run("ufs=0 ufs_native=0 ufs_probe=0") == 0);
    assert(!block_enabled && !hc_requested && !block_probe_requested);
    assert(run("ufs=1") == 1);
    assert(!block_enabled && block_starts == 2 && surveys == 3);
    puts("PASS: actual run argument handling resets storage service/probe/native requests between RAM sessions");
    return 0;
}
