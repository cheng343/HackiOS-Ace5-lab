#include "common.h"

static uint32_t last_step, pause_ms, fine, watch_seconds, dirty, new_from, step;
static bool whole, whole_runs, quiet;
static const char *last_write = "stub";
static int qrtr_ops;
static struct { uint32_t wanted; } radio_space;
static void redwood_qmi_set_transport(const void *ops) { CHECK(ops == &qrtr_ops); calls.transport++; }
static const void *image_file(const char *name, uint64_t *size)
{ CHECK(strcmp(name, "wpss.mdt") == 0); *size = 1; calls.firmware++; return "stub"; }
static void redwood_lx_net_early(void) { calls.network++; }
static int redwood_lx_start(void) { calls.lx++; return -1; }
static uint32_t redwood_wifi_option(const char *name) { return option(name); }
static void smmu_look(void) { calls.smmu++; }
static void radio_space_try(void) { calls.radio++; }
static bool one_step(void) { calls.steps++; return true; }
static bool (*const steps[])(void) = {
    one_step, one_step, one_step, one_step, one_step, one_step, one_step, one_step,
    one_step, one_step, one_step, one_step, one_step, one_step, one_step, one_step
};
#define STEPS ((uint32_t)(sizeof(steps) / sizeof(steps[0])))
static void steps_over(uint32_t done) { CHECK(done == calls.steps); calls.cleanup++; }
#include "wifi-init.inc"

static void reset(const char *name, const char *board)
{
    reset_common(name, board);
    last_step = pause_ms = fine = watch_seconds = dirty = new_from = step = 0;
    whole = whole_runs = quiet = false;
    radio_space.wanted = 0;
}
static void reject(const char *board, bool automatic, bool malformed)
{
    reset("wifi reject before any driver work", board);
    if (malformed) dt_size--;
    arg(automatic ? "wifi" : "wifi_step", 1);
    arg("wifi_lx", 1); arg("wifi_smmu", 2); arg("wifi_return", 1);
    redwood_wifi_init(); expect_idle(); CHECK(last_step == 0);
}
int main(void)
{
    const char *rejected[] = {"giulia", "unknown", "redwoodX"};
    for (unsigned i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        reject(rejected[i], false, false); reject(rejected[i], true, false);
    }
    reject("redwood", false, true); reject("redwood", true, true);
    reset("wifi absent", "redwood"); redwood_wifi_init(); expect_idle();
    reset("wifi invalid value", "redwood"); arg("wifi", 2); redwood_wifi_init(); expect_idle();
    reset("wifi QEMU no lx", "qemu-virt"); arg("wifi_step", 1);
    redwood_wifi_init(); expect_idle(); CHECK(last_step == 0);
    const uint32_t forbidden_lx[] = {0, 2, UINT32_MAX};
    for (unsigned i = 0; i < sizeof(forbidden_lx) / sizeof(forbidden_lx[0]); i++) {
        reset("wifi QEMU rejects lx modes with hardware access", "qemu-virt");
        arg("wifi_step", 1); arg("wifi_lx", forbidden_lx[i]); redwood_wifi_init();
        expect_idle(); CHECK(last_step == 0);
    }
    reject("qemu-virt", true, false);
    reset("wifi QEMU dry driver", "qemu-virt"); arg("wifi_step", 1); arg("wifi_lx", 1);
    arg("wifi_smmu", 2); arg("wifi_return", 1); redwood_wifi_init();
    CHECK(calls.lx == 1); calls.lx = 0; expect_idle(); CHECK(last_step == 0);
    reset("wifi Redwood automatic", "redwood"); arg("wifi", 1); redwood_wifi_init();
    CHECK(calls.transport == 1 && calls.firmware == 1 && calls.network == 1);
    CHECK(calls.threads == 1 && calls.dealloc == 1 && whole && whole_runs);
    CHECK(calls.steps == 0 && calls.lx == 0);
    reset("wifi Redwood one step", "redwood"); arg("wifi_step", 1); arg("wifi_lx", 1);
    arg("wifi_smmu", 2); arg("wifi_return", 1); redwood_wifi_init();
    CHECK(calls.transport == 1 && calls.steps == 1 && calls.cleanup == 1);
    CHECK(calls.smmu == 1 && calls.radio == 1 && calls.returns == 1);
    CHECK(calls.lx == 0 && calls.threads == 0 && calls.firmware == 0);
    puts("PASS: actual Wi-Fi init rejects boards before work and preserves Redwood/QEMU routes");
    return 0;
}
