/* The production functions come from eagl.c, not a second hash implementation. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned GLuint, GLenum;
typedef int GLint, GLsizei;
enum { GL_BGRA = 1, GL_TEXTURE_2D, GL_UNPACK_ROW_LENGTH, GL_UNPACK_ALIGNMENT, kIOSurfaceLockReadOnly };
struct surface { uint8_t *base; size_t row; uint32_t seed; unsigned planes; bool fail_lock; };
typedef struct surface *IOSurfaceRef;
#include "image.inc"
struct eagl {
    unsigned frames, uploads, image_count, unit;
    size_t upload_bytes;
    GLuint bound[8];
    struct image *image;
};
static unsigned gl_uploads;
static uint8_t uploaded_first;
static IOSurfaceRef change_during_upload;
static uint8_t changed_first;
static int trace_placing, eagl_tracing;
static uint32_t advance_seed_on_unlock;
static unsigned bytes_a_pixel(GLenum format, GLenum type) { (void)format; return type; }
static size_t IOSurfaceGetBytesPerRow(IOSurfaceRef s) { return s->row; }
static size_t IOSurfaceGetBytesPerRowOfPlane(IOSurfaceRef s, uint32_t p) { (void)p; return s->row; }
static void *IOSurfaceGetBaseAddress(IOSurfaceRef s) { return s->base; }
static void *IOSurfaceGetBaseAddressOfPlane(IOSurfaceRef s, uint32_t p) { (void)p; return s->base; }
static uint32_t IOSurfaceGetSeed(IOSurfaceRef s) { return s->seed; }
static unsigned IOSurfaceGetPlaneCount(IOSurfaceRef s) { return s->planes; }
static unsigned IOSurfaceGetID(IOSurfaceRef s) { (void)s; return 1; }
static unsigned IOSurfaceGetPixelFormat(IOSurfaceRef s) { (void)s; return 0; }
static size_t IOSurfaceGetBytesPerElement(IOSurfaceRef s) { (void)s; return 4; }
static int IOSurfaceLock(IOSurfaceRef s, int flags, void *seed) { (void)flags; (void)seed; return s->fail_lock; }
static int IOSurfaceUnlock(IOSurfaceRef s, int flags, void *seed) {
    (void)flags; (void)seed;
    s->seed += advance_seed_on_unlock;
    return 0;
}
static void glGetIntegerv(GLenum n, GLint *v) { (void)n; *v = 0; }
static void glPixelStorei(GLenum n, GLint v) { (void)n; (void)v; }
static void glBindTexture(GLenum target, GLuint texture) { (void)target; (void)texture; }
static GLenum internal_format(GLenum internal, GLenum format) { (void)format; return internal; }
static void glTexImage2D(GLenum to, GLint level, GLint internal, GLsizei width, GLsizei height,
    GLint border, GLenum format, GLenum type, const void *base) {
    (void)to; (void)level; (void)internal; (void)width; (void)height;
    (void)border; (void)format; (void)type;
    uploaded_first = ((const uint8_t *)base)[0];
    gl_uploads++;
    if (change_during_upload != NULL) {
        change_during_upload->base[0] = changed_first;
        change_during_upload->seed++;
        change_during_upload = NULL;
    }
}
static void say(const char *format, ...) { (void)format; }
static void eagl_trace(const char *format, ...) { (void)format; }
static struct image *image_of(struct eagl *e, GLuint texture) { return texture == e->image->texture ? e->image : NULL; }
#define EAGL_ENTER(e, offset) ((void)(e), (void)(offset))
#define EAGL_LEAVE(e, offset) ((void)(e), (void)(offset))
#include "functions.inc"

int main(void) {
    /* Exact-size allocations plus ASan also catch reading beyond the short last row. */
    for (unsigned size = 1; size <= 31; size++) {
        uint8_t *pixels = calloc(size, 1);
        assert(pixels != NULL);
        struct surface surface = { .base = pixels, .row = size, .seed = 1 };
        struct image image = { .surface = &surface, .width = (GLsizei)size, .height = 1, .type = 1 };
        struct eagl e = { 0 };
        (void)surface_changed(&e, &image);
        for (unsigned at = 0; at < size; at++) {
            e.frames++;
            pixels[at] ^= 1;
            assert(surface_changed(&e, &image));
            e.frames++;
            assert(!surface_changed(&e, &image));
        }
        free(pixels);
    }
    uint8_t pixels[28] = { 0 }; /* Two odd-width BGRA rows; last row has no padding. */
    struct surface surface = { .base = pixels, .row = 16, .seed = 10 };
    struct image image = { .surface = &surface, .texture = 7, .width = 3, .height = 2,
                          .type = 4, .format = GL_BGRA, .to = GL_TEXTURE_2D };
    struct eagl e = { .image_count = 1, .image = &image };
    surface_to_texture(&e, &image);
    assert(gl_uploads == 1 && image.seed == 10);
    pixels[27] = 1;
    e.frames++;
    my_glBindTexture(&e, GL_TEXTURE_2D, 7);
    assert(gl_uploads == 2); /* Seedless edits of the final pixel are visible. */
    my_glBindTexture(&e, GL_TEXTURE_2D, 7);
    assert(gl_uploads == 2);

    pixels[0] = 2;
    surface.seed++;
    surface_to_texture(&e, &image); /* The seed-driven frame_begin path. */
    assert(gl_uploads == 3);
    my_glBindTexture(&e, GL_TEXTURE_2D, 7);
    e.frames++;
    my_glBindTexture(&e, GL_TEXTURE_2D, 7);
    assert(gl_uploads == 3); /* A stale fingerprint must not upload the same data again. */

    pixels[0] = 3;
    surface.seed++;
    my_glBindTexture(&e, GL_TEXTURE_2D, 7); /* New seed even in an already sampled frame. */
    my_glBindTexture(&e, GL_TEXTURE_2D, 7);
    e.frames++;
    my_glBindTexture(&e, GL_TEXTURE_2D, 7);
    assert(gl_uploads == 4);

    surface.seed++;
    surface.fail_lock = true;
    uint32_t old_seed = image.seed;
    my_glBindTexture(&e, GL_TEXTURE_2D, 7);
    assert(gl_uploads == 4 && image.seed == old_seed);
    surface.fail_lock = false;
    my_glBindTexture(&e, GL_TEXTURE_2D, 7);
    assert(gl_uploads == 5 && image.seed == surface.seed);

    advance_seed_on_unlock = 1;
    old_seed = surface.seed;
    surface_to_texture(&e, &image);
    assert(image.seed == old_seed && surface.seed == old_seed + 1);
    advance_seed_on_unlock = 0;
    my_glBindTexture(&e, GL_TEXTURE_2D, 7);
    assert(gl_uploads == 7); /* Do not acknowledge an edit after releasing the read lock. */

    /* A sampled seedless change must survive a failed lock, even in this frame.
     * Both ordinary textures and not-yet-painted wrapped targets take this path. */
    for (unsigned wrapped = 0; wrapped < 2; wrapped++) {
        unsigned before = gl_uploads;
        e.frames++;
        image.target = image.wrapped = wrapped != 0;
        pixels[0]++;
        surface.fail_lock = true;
        my_glBindTexture(&e, GL_TEXTURE_2D, 7);
        assert(gl_uploads == before && image.upload_pending);
        surface.fail_lock = false;
        my_glBindTexture(&e, GL_TEXTURE_2D, 7);
        assert(gl_uploads == before + 1 && !image.upload_pending && uploaded_first == pixels[0]);
        my_glBindTexture(&e, GL_TEXTURE_2D, 7);
        assert(gl_uploads == before + 1);
    }
    image.target = image.wrapped = false;

    /* A reader lock is not exclusive: the upload can finish with an old copy
     * after another writer changes the seed. The next bind must retry. */
    unsigned before = gl_uploads;
    pixels[0] = 30;
    surface.seed++;
    old_seed = surface.seed;
    changed_first = 31;
    change_during_upload = &surface;
    my_glBindTexture(&e, GL_TEXTURE_2D, 7);
    assert(gl_uploads == before + 1 && uploaded_first == 30);
    assert(image.seed == old_seed && surface.seed == old_seed + 1);
    my_glBindTexture(&e, GL_TEXTURE_2D, 7);
    assert(gl_uploads == before + 2 && uploaded_first == 31 && image.seed == surface.seed);
    my_glBindTexture(&e, GL_TEXTURE_2D, 7);
    assert(gl_uploads == before + 2);

    e.frames++;
    image.height = 0;
    assert(!surface_changed(&e, &image));
    image.height = 2;
    image.width = 0;
    assert(!surface_changed(&e, &image));
    image.width = 3;
    surface.planes = 1;
    assert(!surface_changed(&e, &image));
    surface.planes = 0;
    surface.base = NULL;
    assert(!surface_changed(&e, &image));
    puts("PASS: EAGL hash tails, upload deduplication, seed changes during upload and seedless lock retries (host stubs)");
    return 0;
}
