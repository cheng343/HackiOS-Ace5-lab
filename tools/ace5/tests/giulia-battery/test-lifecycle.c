/* Extracted production GPIO/entry/stop code; MMIO and XNU services are mocks. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../../../../xnu/files/bsd/dev/giulia_battery_bus.h"
#define __unused
#define __printflike(format_index, first_arg) __attribute__((format(printf, format_index, first_arg)))
#define NSEC_PER_USEC 1
#define NSEC_PER_MSEC 1000
#define KERN_SUCCESS 0
#define kSuccess 0
#define HACKIOS_PLATFORM_GIULIA 2
typedef unsigned DTEntry, thread_t;
typedef int wait_result_t;
struct topology { unsigned num_cpus; };
static struct topology topology = {1};
static unsigned preempt, reads, writes, spawned, optin, cpus = 1, bootcpu = 1, board = 2;
static unsigned finish_on_delay, stop_on_delay;
static uint64_t clock_us;
static uint32_t cfg[2] = {4, 4}, io[2] = {1, 1};
static uint64_t gic[4] = {0x16d00000, 0x10000, 0x16d80000, 0x10000};
static void loader_log_line(const char *line) { (void)line; assert(!preempt); }
#include "state.inc"
static void gb_restore(void);
static void disable_preemption(void) { preempt++; }
static void enable_preemption(void) { assert(preempt); preempt--; }
static uint32_t gb_mmio_read(uintptr_t address)
{
    reads++;
    uintptr_t off = address - gb_gpio;
    assert(off == 0 || off == 4 || off == 0x1000 || off == 0x1004);
    return (off & 4) ? io[off >> 12] : cfg[off >> 12];
}
static void gb_mmio_write(uintptr_t address, uint32_t value)
{
    assert(preempt); /* Check and GPIO write must be one non-preemptible region. */
    writes++;
    uintptr_t off = address - gb_gpio;
    assert(off == 0 || off == 4 || off == 0x1000 || off == 0x1004);
    unsigned pin = off >> 12;
    if (off & 4) {
        io[pin] = (io[pin] & 1) | (value & 2);
    } else {
        cfg[pin] = value;
        /* A GPIO output with its high latch set is forbidden. */
        assert(((value >> 2) & 15) || !(value & (1u << 9)) || !(io[pin] & 2));
    }
}
static void delay_for_interval(unsigned count, unsigned scale)
{
    assert(!preempt); /* No delay/sleep inside the GPIO critical sections. */
    clock_us += (uint64_t)count * scale;
    if (stop_on_delay) { gb_phase = GB_STOPPING; stop_on_delay = 0; }
    if (finish_on_delay && gb_phase == GB_STOPPING) {
        gb_restore(); gb_phase = GB_OFF; finish_on_delay = 0;
    }
}
static uint64_t mach_absolute_time(void) { return clock_us; }
static void clock_interval_to_deadline(unsigned count, unsigned scale, uint64_t *out)
{ *out = clock_us + (uint64_t)count * scale; }
static bool PE_parse_boot_argn(const char *key, void *out, unsigned size)
{
    assert(size == 4);
    uint32_t value = !strcmp(key, "ace5_battery") ? optin : !strcmp(key, "cpus") ? cpus : bootcpu;
    memcpy(out, &value, size); return true;
}
static unsigned hackios_platform(void) { return board; }
static int SecureDTLookupEntry(void *from, const char *path, DTEntry *entry)
{ (void)from; assert(!strcmp(path, "/arm-io/gic")); *entry = 1; return 0; }
static int SecureDTGetProperty(DTEntry entry, const char *key, const void **out, unsigned *size)
{ assert(entry == 1 && !strcmp(key, "reg")); *out = gic; *size = sizeof(gic); return 0; }
static uint64_t pe_arm_get_soc_base_phys(void) { return 0x400000; }
static const struct topology *ml_get_topology_info(void) { return &topology; }
static void gb_thread(void *arg, wait_result_t result) { (void)arg; (void)result; }
static int kernel_thread_start(void (*run)(void *, wait_result_t), void *arg, thread_t *thread)
{ assert(run == gb_thread && arg == NULL); spawned++; *thread = 1; return 0; }
static void thread_deallocate(thread_t thread) { assert(thread == 1); }
#include "gpio.inc"
#include "lifecycle.inc"

int main(void)
{
    assert(!giulia_battery_init() && !spawned && !reads && !writes);
    optin = 1; board = 1;
    assert(!giulia_battery_init() && !spawned && !reads && !writes);
    board = 2; bootcpu = 0;
    assert(giulia_battery_init() && !spawned && !reads && !writes);
    bootcpu = 1; cpus = 0;
    assert(giulia_battery_init() && !spawned && !reads && !writes);
    cpus = 8;
    assert(giulia_battery_init() && !spawned && !reads && !writes);
    cpus = 1; topology.num_cpus = 8;
    assert(giulia_battery_init() && !spawned && !reads && !writes);
    cpus = 1; topology.num_cpus = 1; gic[0]++;
    assert(!giulia_battery_init() && !spawned && !reads && !writes);
    gic[0]--;
    assert(giulia_battery_init() && spawned == 1 && !reads && !writes);
    gb_gpio = 0x100000; gb_phase = GB_READING;
    assert(gb_take() == 0 && gb_taken && !preempt);
    assert(cfg[0] == 0 && cfg[1] == 0 && !(io[0] & 2) && !(io[1] & 2));
    gb_set(NULL, GB_SDA, false); assert(cfg[0] == (1u << 9));
    gb_set(NULL, GB_SDA, true); assert(cfg[0] == 0);
    gb_restore(); assert(!gb_taken && cfg[0] == 4 && cfg[1] == 4 && !preempt);

    unsigned before = writes;
    cfg[0] = 8; assert(gb_take() == GB_BUSY && writes == before);
    cfg[0] = (1u << 9); assert(gb_take() == GB_BUSY && writes == before);
    cfg[0] = 4; io[0] = 0; assert(gb_take() == GB_BUSY && writes == before);
    io[0] = 1; stop_on_delay = 1;
    assert(gb_take() == GB_STOPPED && writes == before && !gb_taken);

    gb_phase = GB_READING; assert(gb_take() == 0);
    gb_phase = GB_STOPPING;
    gb_set(NULL, GB_SDA, false); assert(cfg[0] == (1u << 9)); /* In-flight word may drain. */
    uint16_t value = 0xbeef;
    assert(gb_sample_word(NULL, 0x08, &value) == GB_STOPPED); /* Next word cannot start. */
    giulia_battery_quiesce();
    assert(gb_phase == GB_OFF && !gb_taken && cfg[0] == 4 && cfg[1] == 4 && !preempt);
    before = writes;
    gb_set(NULL, GB_SDA, false); gb_set(NULL, GB_SCL, false);
    assert(writes == before); /* Resumed worker cannot write after final quiesce. */

    gb_phase = GB_READING; assert(gb_take() == 0); finish_on_delay = 1;
    giulia_battery_stop();
    assert(gb_phase == GB_OFF && !gb_taken && !preempt);
    gb_phase = GB_IDLE; before = writes;
    giulia_battery_stop(); assert(gb_phase == GB_OFF && writes == before);

    gb_phase = GB_READING; assert(gb_take() == 0);
    uint64_t began = clock_us;
    giulia_battery_stop(); /* Simulate a descheduled/stuck worker; bounded wait. */
    assert(clock_us - began == 250000 && gb_phase == GB_STOPPING);
    giulia_battery_quiesce(); assert(!gb_taken && cfg[0] == 4 && cfg[1] == 4);
    before = writes; giulia_battery_quiesce(); assert(writes == before && !preempt);
    puts("PASS: real battery entry guards, open-drain GPIO, mux restore, stop drain/deadline and final quiesce");
    return 0;
}
