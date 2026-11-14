#include "common.h"

static uint32_t level, probe_level;
static void probe_thread(void *argument, int result)
{
    (void)argument; (void)result; CHECK(false);
}
static void haptic_thread(void *argument, int result)
{
    (void)argument; (void)result; CHECK(false);
}
static void torch_thread(void *argument, int result)
{
    (void)argument; (void)result; CHECK(false);
}
#include "speaker-init.inc"
#include "haptic-init.inc"
#include "torch-init.inc"

struct peripheral {
    const char *option;
    void (*init)(void);
    uint32_t *requested;
    uint32_t maximum;
};

static void reset(const char *name, const char *board)
{
    reset_common(name, board);
    level = probe_level = 0;
}

static void denied(const struct peripheral *peripheral)
{
    peripheral->init();
    expect_idle();
    CHECK(*peripheral->requested == 0);
}

int main(void)
{
    const struct peripheral peripherals[] = {
        {"speaker_probe", redwood_speaker_init, &probe_level, UINT32_MAX},
        {"haptic", redwood_haptic_init, &level, 5},
        {"torch", redwood_torch_init, &level, 3},
    };
    const char *boards[] = {"redwood", "giulia", "qemu-virt", "unknown", "redwoodX"};
    const uint32_t levels[] = {1, 2, 3, 5, UINT32_MAX};

    for (unsigned p = 0; p < sizeof(peripherals) / sizeof(peripherals[0]); p++) {
        const struct peripheral *peripheral = &peripherals[p];
        for (unsigned b = 0; b < sizeof(boards) / sizeof(boards[0]); b++) {
            reset(peripheral->option, boards[b]);
            denied(peripheral);  /* Absent boot argument must not start anything. */
            reset(peripheral->option, boards[b]);
            arg(peripheral->option, 0);
            denied(peripheral);

            for (unsigned v = 0; v < sizeof(levels) / sizeof(levels[0]); v++) {
                reset(peripheral->option, boards[b]);
                arg(peripheral->option, levels[v]);
                peripheral->init();
                if (b != 0) {
                    expect_idle();
                    CHECK(*peripheral->requested == 0);
                } else {
                    CHECK(calls.threads == 1 && calls.dealloc == 1);
                    CHECK(*peripheral->requested == (levels[v] > peripheral->maximum ?
                        peripheral->maximum : levels[v]));
                    calls.threads = calls.dealloc = 0;
                    expect_idle();
                }
            }
        }

        reset(peripheral->option, "redwood"); dt_missing_node = true;
        arg(peripheral->option, 1); denied(peripheral);
        reset(peripheral->option, "redwood"); dt_missing_property = true;
        arg(peripheral->option, 1); denied(peripheral);
        reset(peripheral->option, NULL);
        arg(peripheral->option, 1); denied(peripheral);
        reset(peripheral->option, "redwood"); dt_size--;  /* Missing terminal NUL. */
        arg(peripheral->option, 1); denied(peripheral);
        reset(peripheral->option, "redwood\0extra"); dt_size = sizeof("redwood\0extra");
        arg(peripheral->option, 1); denied(peripheral);
    }
    puts("PASS: actual speaker, haptic and torch init reject other boards before worker start; Redwood modes retained");
    return 0;
}
