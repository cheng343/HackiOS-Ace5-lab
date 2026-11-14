/* Execute the real worker with both terminating functions allowed to return.
 * This catches a fake-camera fallthrough even though no actual hardware exists.
 */
#define TEST_REAL_CAMERA_THREAD
#include "common.h"
#define __unused __attribute__((unused))
typedef int wait_result_t;
static uint32_t level;
static bool fake;
static uintptr_t tlmm, gcc, camcc, arb_core, arb_cnfg, arb_observer;
enum { TLMM_PHYS = 1, TLMM_SIZE, GCC_PHYS, CAMCC_PHYS, CAMCC_SIZE,
       ARB_CORE_PHYS, ARB_CORE_SIZE, ARB_CNFG_PHYS, ARB_CNFG_SIZE,
       ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE };
static unsigned hardware_calls, io_maps, served, terminated, devices, selections;
static const struct { const char *name; } sensors[] = {{"macro camera"}, {"wide camera"}};
static struct { const char *name; } selected;
#define hw (&selected)
#define say silent_log
static bool PE_parse_boot_arg_str(const char *name, char *value, unsigned size)
{ (void)name; (void)value; (void)size; return false; }
static void camdev_make(void) { devices++; }
static void select_sensor(unsigned index)
{ CHECK(index < 2); selected.name = sensors[index].name; selections++; }
static void serve_ios(void) { served++; }
static thread_t current_thread(void) { return 1; }
static void thread_terminate(thread_t thread) { CHECK(thread == 1); terminated++; }
static void wait_ms(uint32_t milliseconds) { (void)milliseconds; hardware_calls++; }
static uintptr_t ml_io_map(uintptr_t address, unsigned size)
{ CHECK(address != 0 && size != 0); io_maps++; hardware_calls++; return address; }
#define HARDWARE_VOID(name) static void name(void) { hardware_calls++; }
#define HARDWARE_BOOL(name) static bool name(void) { hardware_calls++; return true; }
HARDWARE_VOID(pgpio_find)
HARDWARE_VOID(find_pins)
HARDWARE_VOID(look)
HARDWARE_VOID(look_around)
HARDWARE_VOID(capture_tpg)
HARDWARE_VOID(capture_sensor)
HARDWARE_VOID(power_down)
HARDWARE_VOID(show_ldos)
HARDWARE_BOOL(power_up)
HARDWARE_BOOL(healthy)
HARDWARE_BOOL(read_chip_id)
static bool ctab_load(void) { return true; } /* Local table parsing only. */
#include "camera-thread.inc"

static void reset_worker(bool fake_value)
{
    reset_common(fake_value ? "camera fake worker must return before MMIO" :
                 "camera hardware control reaches counter stubs", "qemu-virt");
    level = 5; fake = fake_value;
    hardware_calls = io_maps = served = terminated = devices = selections = 0;
    tlmm = gcc = camcc = arb_core = arb_cnfg = arb_observer = 0;
    arg("cam_after", 0);
}
int main(void)
{
    reset_worker(true); camera_thread(NULL, 0);
    CHECK(served == 1 && terminated == 1 && devices == 1 && selections == 1);
    CHECK(hardware_calls == 0 && io_maps == 0);
    CHECK(tlmm == 0 && gcc == 0 && camcc == 0 && arb_core == 0 && arb_cnfg == 0 && arb_observer == 0);
    reset_worker(false); camera_thread(NULL, 0);
    CHECK(io_maps == 6 && hardware_calls > 6 && served == 1 && terminated == 1);
    puts("PASS: actual camera fake worker returns before MMIO even if thread_terminate returns");
    return 0;
}
