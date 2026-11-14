#include "common.h"

int main(void)
{
    const struct { const char *name; enum hackios_platform expected; } known[] = {
        {"redwood", HACKIOS_PLATFORM_REDWOOD}, {"giulia", HACKIOS_PLATFORM_GIULIA},
        {"qemu-virt", HACKIOS_PLATFORM_QEMU_VIRT}, {"", HACKIOS_PLATFORM_UNKNOWN},
        {"redwoodX", HACKIOS_PLATFORM_UNKNOWN}, {"Redwood", HACKIOS_PLATFORM_UNKNOWN}
    };
    for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        reset_common(known[i].name, known[i].name);
        CHECK(hackios_platform() == known[i].expected);
    }
    reset_common("missing node", "redwood"); dt_missing_node = true;
    CHECK(hackios_platform() == HACKIOS_PLATFORM_UNKNOWN);
    reset_common("missing property", "redwood"); dt_missing_property = true;
    CHECK(hackios_platform() == HACKIOS_PLATFORM_UNKNOWN);
    reset_common("NULL property", NULL); dt_size = 8;
    CHECK(hackios_platform() == HACKIOS_PLATFORM_UNKNOWN);
    reset_common("zero length", "redwood"); dt_size = 0;
    CHECK(hackios_platform() == HACKIOS_PLATFORM_UNKNOWN);
    static const char unterminated[] = {'r','e','d','w','o','o','d','X'};
    reset_common("unterminated exact size", "redwood"); dt_value = unterminated;
    CHECK(hackios_platform() == HACKIOS_PLATFORM_UNKNOWN);
    static const char padded[] = "redwood\0junk";
    reset_common("trailing garbage", "redwood"); dt_value = padded; dt_size = sizeof(padded);
    CHECK(hackios_platform() == HACKIOS_PLATFORM_UNKNOWN);
    const char *boards[] = {"redwood", "giulia", "qemu-virt"};
    for (unsigned i = 0; i < sizeof(boards) / sizeof(boards[0]); i++) {
        reset_common("all truncated lengths", boards[i]);
        for (unsigned size = 0; size <= strlen(boards[i]); size++) {
            dt_size = size;
            CHECK(hackios_platform() == HACKIOS_PLATFORM_UNKNOWN);
        }
    }
    expect_idle();
    puts("PASS: platform parser exact names and malformed/missing DT");
    return 0;
}
