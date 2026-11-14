#include "common.h"
static uint32_t level;
static bool fake;
static vm_offset_t keep;
static uint64_t keep_bytes;
static void *kernel_map;
static int kmem_alloc_contig(void *map, vm_offset_t *address, uint64_t bytes,
    unsigned mask, unsigned max_page, unsigned boundary, unsigned flags, unsigned tag)
{
    CHECK(map == kernel_map && bytes > 0 && mask == PAGE_MASK);
    CHECK(max_page == 0 && boundary == 0 && flags == (KMA_KOBJECT | KMA_ZERO | KMA_DATA));
    CHECK(tag == VM_KERN_MEMORY_IOKIT);
    calls.allocations++; *address = 1; return KERN_SUCCESS;
}
#include "camera-init.inc"

static void reset(const char *name, const char *board)
{
    reset_common(name, board); level = 0; fake = false; keep = 0; keep_bytes = 0;
}
int main(void)
{
    const char *rejected[] = {"giulia", "unknown", "redwoodX"};
    for (unsigned i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        for (unsigned mode = 1; mode <= 6; mode++) {
            reset("camera unsupported board including fake", rejected[i]);
            arg("cam", mode); arg("cam_fake", 1); redwood_camera_init();
            expect_idle(); CHECK(level == 0 && !fake && keep_bytes == 0);
        }
    }
    reset("camera truncated DT", "redwood"); dt_size--; arg("cam", 5); arg("cam_fake", 1);
    redwood_camera_init(); expect_idle(); CHECK(level == 0 && !fake);
    reset("camera absent", "redwood"); redwood_camera_init(); expect_idle();
    reset("camera disabled", "redwood"); arg("cam", 0); redwood_camera_init(); expect_idle();
    for (unsigned mode = 1; mode <= 5; mode++) {
        reset("camera QEMU hardware rejected", "qemu-virt"); arg("cam", mode);
        if (mode < 5) arg("cam_fake", 1); /* fake must never bypass modes 1..4. */
        redwood_camera_init(); expect_idle(); CHECK(level == 0 && !fake);
        reset("camera Redwood hardware", "redwood"); arg("cam", mode); redwood_camera_init();
        CHECK(calls.threads == 1 && calls.dealloc == 1 && !fake);
        CHECK(calls.allocations == (mode >= 3 ? 1u : 0u));
    }
    const char *fake_boards[] = {"qemu-virt", "redwood"};
    for (unsigned i = 0; i < 2; i++) {
        for (unsigned mode = 5; mode <= 6; mode++) {
            reset("camera fake preserved with existing clamp", fake_boards[i]);
            arg("cam", mode); arg("cam_fake", 1); redwood_camera_init();
            CHECK(level == 5 && fake && calls.allocations == 1);
            CHECK(calls.threads == 1 && calls.dealloc == 1);
        }
    }
    puts("PASS: actual camera init rejects before allocation/thread and preserves fake routes");
    return 0;
}
