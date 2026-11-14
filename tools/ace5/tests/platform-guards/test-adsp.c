#include "common.h"
#define STEPS 11
static uint32_t last_step, pause_ms, listen_seconds, new_from;
static bool keep, quiet;
static void run_steps(void) { calls.steps++; }
#include "adsp-init.inc"

static void reset(const char *name, const char *board)
{
    reset_common(name, board);
    last_step = pause_ms = listen_seconds = new_from = 0;
    keep = quiet = false;
}
int main(void)
{
    const char *rejected[] = {"giulia", "qemu-virt", "unknown", "redwoodX"};
    for (unsigned i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        for (unsigned mode = 0; mode < 2; mode++) {
            reset("ADSP unsupported board", rejected[i]);
            arg(mode ? "adsp" : "adsp_step", 1); arg("adsp_return", 1);
            redwood_adsp_init(); expect_idle(); CHECK(last_step == 0);
        }
    }
    reset("ADSP truncated DT", "redwood"); dt_size--; arg("adsp_step", 1);
    redwood_adsp_init(); expect_idle(); CHECK(last_step == 0);
    reset("ADSP absent", "redwood"); redwood_adsp_init(); expect_idle();
    reset("ADSP invalid value", "redwood"); arg("adsp", 2); redwood_adsp_init(); expect_idle();
    reset("ADSP Redwood automatic", "redwood"); arg("adsp", 1); redwood_adsp_init();
    CHECK(calls.threads == 1 && calls.dealloc == 1 && calls.steps == 0);
    CHECK(last_step == STEPS && keep && quiet);
    reset("ADSP Redwood steps", "redwood"); arg("adsp_step", 1); arg("adsp_return", 1);
    redwood_adsp_init(); CHECK(calls.steps == 1 && calls.returns == 1 && calls.threads == 0);
    puts("PASS: actual ADSP init rejects boards before work and preserves Redwood routes");
    return 0;
}
