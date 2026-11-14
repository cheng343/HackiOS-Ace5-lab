/* Host-only stubs. Thread start does not execute callbacks; no MMIO is available. */
#ifndef PLATFORM_GUARD_TEST_COMMON_H
#define PLATFORM_GUARD_TEST_COMMON_H
#ifdef NDEBUG
#error "Assertions must be enabled: pass -UNDEBUG after optimization flags"
#endif
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/hackios_platform.h>

static const char *case_name;
#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "%s:%d: %s: failed: %s\n", __FILE__, __LINE__, case_name, #expression); \
        exit(1); \
    } \
} while (0)

static const void *dt_value;
static unsigned dt_size;
static bool dt_missing_node, dt_missing_property;
int SecureDTLookupEntry(DTEntry start, const char *path, DTEntry *entry)
{
    CHECK(start == NULL);
    CHECK(strcmp(path, "/arm-io") == 0);
    if (dt_missing_node) return kError;
    *entry = (DTEntry)(uintptr_t)1;
    return kSuccess;
}
int SecureDTGetProperty(DTEntry entry, const char *name, const void **value, unsigned *size)
{
    CHECK(entry == (DTEntry)(uintptr_t)1);
    CHECK(strcmp(name, "device_type") == 0);
    if (dt_missing_property) return kError;
    *value = dt_value;
    *size = dt_size;
    return kSuccess;
}

struct boot_arg { const char *name; uint32_t value; };
static struct boot_arg args[16];
static unsigned arg_count;
static void arg(const char *name, uint32_t value)
{
    CHECK(arg_count < sizeof(args) / sizeof(args[0]));
    args[arg_count++] = (struct boot_arg){name, value};
}
static bool PE_parse_boot_argn(const char *name, void *value, unsigned size)
{
    CHECK(size == sizeof(uint32_t));
    for (unsigned i = 0; i < arg_count; i++) {
        if (strcmp(name, args[i].name) == 0) {
            memcpy(value, &args[i].value, sizeof(args[i].value));
            return true;
        }
    }
    return false;
}
static uint32_t option(const char *name)
{
    uint32_t value = 0;
    PE_parse_boot_argn(name, &value, sizeof(value));
    return value;
}

struct counters {
    unsigned transport, firmware, network, lx, threads, dealloc, allocations;
    unsigned smmu, radio, steps, cleanup, returns;
};
static struct counters calls;
typedef uintptr_t thread_t;
typedef uintptr_t vm_offset_t;
typedef void (*thread_fn)(void *, int);
enum { KERN_SUCCESS = 0, KMA_KOBJECT = 1, KMA_ZERO = 2, KMA_DATA = 4,
       VM_KERN_MEMORY_IOKIT = 0 };
#define PAGE_MASK 0x3fff
static int kernel_thread_start(thread_fn function, void *context, thread_t *thread)
{
    CHECK(function != NULL && context == NULL);
    calls.threads++;
    *thread = 1;
    return KERN_SUCCESS;
}
static void thread_deallocate(thread_t thread) { CHECK(thread == 1); calls.dealloc++; }
static void loader_return(void) { calls.returns++; }
static void whole_thread(void *arg_value, int result) { (void)arg_value; (void)result; CHECK(false); }
#ifndef TEST_REAL_CAMERA_THREAD
static void camera_thread(void *arg_value, int result) { (void)arg_value; (void)result; CHECK(false); }
#endif
static int silent_log(const char *format, ...) { (void)format; return 0; }
#define printf silent_log
#define tell silent_log

static void reset_common(const char *name, const char *board)
{
    case_name = name;
    dt_value = board;
    dt_size = board ? (unsigned)strlen(board) + 1 : 0;
    dt_missing_node = dt_missing_property = false;
    arg_count = 0;
    memset(&calls, 0, sizeof(calls));
    assert(arg_count == 0); /* NDEBUG above must never bypass this assertion. */
}
static void expect_idle(void)
{
    const struct counters zero = {0};
    CHECK(memcmp(&calls, &zero, sizeof(calls)) == 0);
}
#endif
