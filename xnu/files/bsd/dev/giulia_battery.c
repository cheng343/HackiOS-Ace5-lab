/* Experimental Ace 5 PKG110 NFG8011B telemetry. Explicit ace5_battery=1 only.
 * Current live DT: I2C 11, QUP2 SE0 @ 0x880000, slave 0x55, single cell,
 * GPIO0=SDA / GPIO1=SCL. Android calls the multi-chip driver bq27541-battery.
 * TLMM facts: Linux drivers/pinctrl/qcom/pinctrl-sm8650.c; mux[5:2], OE[9],
 * input[0], output[1], GPIO stride 0x1000, bank offset 0x100000.
 * This uses open-drain GPIO I2C and restores the original mux after each poll.
 * No GENI clocks, regulator, PMIC, charger, gauge configuration or flash writes.
 * Not validated on hardware; no initial cached/default readings are published.
 */
#include <kern/clock.h>
#include <kern/cpu_data.h> /* XNU 8796: disable_preemption()/enable_preemption() */
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <sys/redwood_input.h>
#include <sys/hackios_platform.h>
#include <sys/systm.h>
#include "giulia_battery_bus.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern vm_offset_t pe_arm_get_soc_base_phys(void);
extern kern_return_t thread_terminate(thread_t);
extern void redwood_battery_gauge_report(const struct redwood_battery_state *);
extern void loader_log_line(const char *);
enum { GB_IDLE = 1, GB_READING, GB_STOPPING, GB_OFF };
static unsigned gb_phase;
static uintptr_t gb_gpio;
static uint32_t gb_saved_cfg[2], gb_saved_io[2], gb_input_cfg[2];
static bool gb_taken;

/* printf's message buffer is appended only when returning to the loader.
 * Mirror diagnostics into its live ring without changing sampling cadence. */
static void gb_log(const char *format, ...) __printflike(1, 2);
static void gb_log(const char *format, ...)
{
    char line[256];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    loader_log_line(line);
    printf("%s", line);
}

static uint32_t gb_mmio_read(uintptr_t address) { return *(volatile uint32_t *)address; }
static void gb_mmio_write(uintptr_t address, uint32_t value)
{
    *(volatile uint32_t *)address = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
static bool gb_running(void *context)
{
    (void)context;
    unsigned phase = __atomic_load_n(&gb_phase, __ATOMIC_ACQUIRE);
    /* A normal stop drains the current bounded read-word including its STOP.
     * STOPPING blocks the next register read, not the edges of one in flight. */
    return phase == GB_READING || phase == GB_STOPPING;
}
static void gb_set(void *context, unsigned pin, bool release)
{
    (void)context;
    disable_preemption();
    /* Output data is always zero. High is produced by the board's pull-up,
     * with the pin in input mode, never by pushing a high level. */
    if (gb_taken && gb_running(NULL))
        gb_mmio_write(gb_gpio + pin * 0x1000, gb_input_cfg[pin] | (release ? 0 : 1u << 9));
    enable_preemption();
}
static bool gb_get(void *context, unsigned pin)
{
    (void)context;
    disable_preemption();
    bool high = gb_running(NULL) && (gb_mmio_read(gb_gpio + pin * 0x1000 + 4) & 1);
    enable_preemption();
    return high;
}
static void gb_delay(void *context, unsigned us)
{
    (void)context;
    delay_for_interval(us, NSEC_PER_USEC);
}
static void gb_restore(void)
{
    disable_preemption();
    if (!gb_taken) {
        enable_preemption();
        return;
    }
    /* Release both wires before restoring the original function/data latch. */
    for (unsigned pin = 0; pin < 2; pin++)
        gb_mmio_write(gb_gpio + pin * 0x1000, gb_input_cfg[pin]);
    for (unsigned pin = 0; pin < 2; pin++) {
        gb_mmio_write(gb_gpio + pin * 0x1000 + 4, gb_saved_io[pin]);
        gb_mmio_write(gb_gpio + pin * 0x1000, gb_saved_cfg[pin]);
    }
    gb_taken = false;
    enable_preemption();
}
static int gb_take(void)
{
    if (__atomic_load_n(&gb_phase, __ATOMIC_ACQUIRE) != GB_READING) return GB_STOPPED;
    /* Save/validate both pins before changing either. Only GPIO inputs or the
     * documented QUP2 SE0 function (mux 1) are accepted; do not take an output. */
    for (unsigned pin = 0; pin < 2; pin++) {
        uintptr_t address = gb_gpio + pin * 0x1000;
        uint32_t cfg = gb_mmio_read(address), mux = (cfg >> 2) & 15;
        if (mux > 1 || (!mux && (cfg & (1u << 9)))) return GB_BUSY;
        gb_saved_cfg[pin] = cfg;
        gb_saved_io[pin] = gb_mmio_read(address + 4) & 2;
        gb_input_cfg[pin] = cfg & ~((15u << 2) | (1u << 9));
    }
    /* Do not steal a visibly active/stuck bus or clock-pulse a slave to reset it. */
    for (unsigned i = 0; i < 10; i++) {
        if (!gb_running(NULL)) return GB_STOPPED;
        if (!gb_get(NULL, GB_SDA) || !gb_get(NULL, GB_SCL)) return GB_BUSY;
        gb_delay(NULL, 5);
    }
    disable_preemption();
    if (__atomic_load_n(&gb_phase, __ATOMIC_ACQUIRE) != GB_READING) {
        enable_preemption();
        return GB_STOPPED;
    }
    gb_taken = true;
    for (unsigned pin = 0; pin < 2; pin++) {
        /* First input, then preload a low latch; no high output transition. */
        gb_mmio_write(gb_gpio + pin * 0x1000, gb_input_cfg[pin]);
        gb_mmio_write(gb_gpio + pin * 0x1000 + 4, 0);
    }
    enable_preemption();
    return GB_OK;
}
static int gb_sample_word(void *context, uint8_t reg, uint16_t *value)
{
    if (__atomic_load_n(&gb_phase, __ATOMIC_ACQUIRE) != GB_READING) return GB_STOPPED;
    return gb_read_word(context, reg, value);
}
static void gb_thread(__unused void *arg, __unused wait_result_t wait)
{
    struct gb_bus bus = {NULL, gb_set, gb_get, gb_delay, gb_running};
    unsigned polls = 0, failures = 0;
    /* A stop before the worker was scheduled must not start hardware work. */
    if (__atomic_load_n(&gb_phase, __ATOMIC_ACQUIRE) != GB_IDLE) goto out;
    gb_gpio = ml_io_map(0x0f100000, 0x2000);
    if (!gb_gpio) goto out;
    for (;;) {
        unsigned expected = GB_IDLE;
        if (!__atomic_compare_exchange_n(&gb_phase, &expected, GB_READING, false,
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) break;
        struct redwood_battery_state report;
        int rc = gb_take();
        if (!rc) rc = gb_sample(gb_sample_word, &bus, &report);
        gb_restore();
        /* Restore first, report only if the whole sample completed and no stop intervened. */
        if (!rc && __atomic_load_n(&gb_phase, __ATOMIC_ACQUIRE) == GB_READING)
            redwood_battery_gauge_report(&report);
        else failures++;
        if (polls++ < 3 || polls % 12 == 0)
            gb_log("ace5-battery: poll=%u rc=%d failures=%u%s\n", polls, rc, failures,
                !rc ? "; gauge sample only, charger state unknown" : "; no sample published");
        expected = GB_READING;
        if (!__atomic_compare_exchange_n(&gb_phase, &expected, GB_IDLE, false,
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) break;
        delay_for_interval(5000, NSEC_PER_MSEC);
    }
out:
    gb_restore();
    __atomic_store_n(&gb_phase, GB_OFF, __ATOMIC_RELEASE);
    thread_terminate(current_thread());
}
/* Called in a normal thread before the return disables interrupts. No mutex
 * can wedge a return: a stop drains the current bounded read-word, and waiting
 * has a 250 ms deadline. The final quiesce handles an interrupted worker too. */
void giulia_battery_stop(void)
{
    unsigned previous = __atomic_exchange_n(&gb_phase, GB_STOPPING, __ATOMIC_ACQ_REL);
    if (previous != GB_READING) {
        __atomic_store_n(&gb_phase, GB_OFF, __ATOMIC_RELEASE);
        return;
    }
    uint64_t deadline;
    clock_interval_to_deadline(250, NSEC_PER_MSEC, &deadline);
    while (__atomic_load_n(&gb_phase, __ATOMIC_ACQUIRE) != GB_OFF && mach_absolute_time() < deadline)
        delay_for_interval(1, NSEC_PER_MSEC);
    if (__atomic_load_n(&gb_phase, __ATOMIC_ACQUIRE) != GB_OFF)
        gb_log("ace5-battery: stop deadline; final quiesce will restore the bus\n");
}
/* Last return path, including panic: only a single XNU CPU is supported.
 * Normal single-CPU loader_return does not necessarily disable interrupts yet.
 * Keep OFF + restoration atomic with respect to worker scheduling. Each worker
 * GPIO change also checks the phase in a non-preemptible section, so a worker
 * resumed after this point cannot perform a previously authorized GPIO write.
 * No sleep/lock/allocator is used here.
 * A panic in a transaction can leave a slave mid-byte: restoring GPIO state
 * is not a claim that the I2C protocol is idle; this path sends no recovery clocks. */
void giulia_battery_quiesce(void)
{
    disable_preemption();
    __atomic_store_n(&gb_phase, GB_OFF, __ATOMIC_RELEASE);
    gb_restore();
    __atomic_store_n(&gb_phase, GB_OFF, __ATOMIC_RELEASE);
    enable_preemption();
}
bool giulia_battery_init(void)
{
    uint32_t enabled = 0, cpus = 0, bootcpu = 0;
    DTEntry entry; const void *property; unsigned size;
    thread_t thread;
    if (!PE_parse_boot_argn("ace5_battery", &enabled, sizeof(enabled)) || enabled != 1) return false;
    if (hackios_platform() != HACKIOS_PLATFORM_GIULIA) return false;
    /* Strict loader board identity first; the GIC map is only a second check.
     * No MMIO is mapped or accessed before all the opt-in/board/CPU checks. */
    if (SecureDTLookupEntry(NULL, "/arm-io/gic", &entry) != kSuccess ||
        SecureDTGetProperty(entry, "reg", &property, &size) != kSuccess || size < 4 * sizeof(uint64_t)) return false;
    const uint64_t *reg = property;
    if (pe_arm_get_soc_base_phys() + reg[0] != 0x17100000 ||
        pe_arm_get_soc_base_phys() + reg[2] != 0x17180000) return false;
    PE_parse_boot_argn("cpus", &cpus, sizeof(cpus));
    PE_parse_boot_argn("bootcpu", &bootcpu, sizeof(bootcpu));
    if (cpus != 1 || bootcpu != 1 || ml_get_topology_info()->num_cpus != 1) {
        gb_log("ace5-battery: experimental GPIO transport requires cpus=1 bootcpu=1\n");
        return true;
    }
    __atomic_store_n(&gb_phase, GB_IDLE, __ATOMIC_RELEASE);
    if (kernel_thread_start(gb_thread, NULL, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else __atomic_store_n(&gb_phase, GB_OFF, __ATOMIC_RELEASE);
    return true;
}
