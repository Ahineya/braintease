/* SPDX-License-Identifier: MIT */
/* Direct C16 guest contract checks for generic retained GX rendering. */
#include <gx.h>
#include "plot-raster.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

static int graphics;
static uint32_t owner;

static struct gx_render request(unsigned operation, uint32_t target) {
    struct gx_render r;
    memset(&r, 0, sizeof(r));
    r.version = GX_VERSION;
    r.operation = operation;
    r.window = owner;
    r.target = target;
    return r;
}
static uint32_t create(unsigned width, unsigned height, unsigned format) {
    struct gx_render r = request(GX_RENDER_CREATE, 0);
    r.width = width; r.height = height; r.format = format;
    assert(!ioctl(graphics, GX_RENDER, &r));
    assert(r.target);
    return r.target;
}
static void destroy(uint32_t target) {
    struct gx_render r = request(GX_RENDER_DESTROY, target);
    assert(!ioctl(graphics, GX_RENDER, &r));
}
static void transfer(unsigned operation, uint32_t target, unsigned width, unsigned height,
                     unsigned long stride, uint8_t *data, unsigned long cells) {
    struct gx_render r = request(operation, target);
    r.width = width; r.height = height; r.stride = stride;
    r.data = data; r.data_length = cells;
    assert(!ioctl(graphics, GX_RENDER, &r));
}
static void batch(uint32_t target, const uint32_t *commands, unsigned long count,
                  uint8_t *data, unsigned long cells) {
    struct gx_render r = request(GX_RENDER_BATCH, target);
    r.commands = commands; r.count = count; r.data = data; r.data_length = cells;
    assert(!ioctl(graphics, GX_RENDER, &r));
}
static void command(uint32_t *p, unsigned operation, unsigned x, unsigned y,
                    unsigned width, unsigned height, uint32_t colour, unsigned opacity) {
    memset(p, 0, GX_RENDER_COMMAND_WORDS * sizeof(*p));
    p[0] = operation; p[1] = x; p[2] = y; p[3] = width; p[4] = height;
    p[5] = colour; p[6] = opacity;
}
static void fill(uint32_t target, unsigned width, unsigned height, uint32_t colour) {
    uint32_t c[GX_RENDER_COMMAND_WORDS];
    command(c, GX_RENDER_FILL, 0, 0, width, height, colour, 255);
    batch(target, c, 1, NULL, 0);
}
static void rejected(struct gx_render *r, unsigned error) {
    errno = 0;
    if (ioctl(graphics, GX_RENDER, r) >= 0 || errno != error) {
        fprintf(stderr, "RENDER_ERROR op=%u got=%u expected=%u\n", r->operation, errno, error);
        assert(0);
    }
}
static void unchanged(uint32_t target, const uint8_t *expected, unsigned width, unsigned height) {
    unsigned long n = (unsigned long)width * height * 3;
    uint8_t *actual = malloc(n);
    assert(actual);
    transfer(GX_RENDER_READBACK, target, width, height, width * 3UL, actual, n);
    assert(!memcmp(actual, expected, n));
    free(actual);
}
static unsigned channel(unsigned old, unsigned colour, unsigned alpha, unsigned opacity) {
    unsigned long mix = opacity >= 253 ? alpha : ((unsigned long)alpha * opacity) >> 8;
    if (!mix) return old;
    if (mix >= 253) return colour;
    return ((unsigned long)colour * mix + (unsigned long)old * (255 - mix)) >> 8;
}
static void roundtrip(unsigned format) {
    unsigned width = 513, height = 129, channels = format == GX_RENDER_A8 ? 1 : 3;
    unsigned long stride = width * (unsigned long)channels + 7;
    unsigned long cells = stride * height + 11, i;
    uint8_t *source = malloc(cells), *actual = malloc(cells), *expected = malloc(cells);
    uint32_t target = create(width, height, format);
    assert(source && actual && expected);
    for (i = 0; i < cells; i++) source[i] = (i * 37 + i / 7) & 511;
    memset(actual, 497, cells);
    memcpy(expected, actual, cells);
    for (i = 0; i < height; i++) {
        unsigned long x;
        for (x = 0; x < width * (unsigned long)channels; x++)
            expected[i * stride + x] = source[i * stride + x] & 255;
    }
    transfer(GX_RENDER_UPLOAD, target, width, height, stride, source, cells);
    transfer(GX_RENDER_READBACK, target, width, height, stride, actual, cells);
    assert(!memcmp(actual, expected, cells));
    /* Input is borrowed and snapshots logical octets; high cell bits and row
     * padding never become part of the native target representation. */
    for (i = 0; i < cells; i++) assert(source[i] == ((i * 37 + i / 7) & 511));
    destroy(target);
    free(source); free(actual); free(expected);
}
static void alpha(void) {
    unsigned width = 256, height = 256, x, y;
    unsigned long cells = width * (unsigned long)height * 3;
    uint8_t *mask = malloc(width * (unsigned long)height), *pixels = malloc(cells);
    uint32_t *commands = calloc(256UL * GX_RENDER_COMMAND_WORDS, sizeof(*commands));
    uint32_t target = create(width, height, GX_XRGB8888);
    assert(mask && pixels && commands);
    fill(target, width, height, 0x307bc9UL);
    for (y = 0; y < height; y++) {
        uint32_t *c = commands + (unsigned long)y * GX_RENDER_COMMAND_WORDS;
        command(c, GX_RENDER_A8_DATA, 0, y, width, 1, 0xe13a85UL, y);
        c[7] = (unsigned long)y * width; c[8] = width;
        for (x = 0; x < width; x++) mask[(unsigned long)y * width + x] = x | 256;
    }
    batch(target, commands, 256, mask, width * (unsigned long)height);
    transfer(GX_RENDER_READBACK, target, width, height, width * 3UL, pixels, cells);
    for (y = 0; y < height; y++) for (x = 0; x < width; x++) {
        uint8_t *p = pixels + ((unsigned long)y * width + x) * 3;
        assert(p[0] == channel(0xc9, 0x85, x, y));
        assert(p[1] == channel(0x7b, 0x3a, x, y));
        assert(p[2] == channel(0x30, 0xe1, x, y));
    }
    /* Unmasked fills use opacity directly. Treating them as mask=255 loses
     * one alpha step below 253 and changes pixels when fills are layered. */
    fill(target, width, height, 0x307bc9UL);
    for (y = 0; y < height; y++)
        command(commands + (unsigned long)y * GX_RENDER_COMMAND_WORDS,
                GX_RENDER_FILL, 0, y, width, 1, 0xe13a85UL, y);
    batch(target, commands, 256, NULL, 0);
    transfer(GX_RENDER_READBACK, target, width, height, width * 3UL, pixels, cells);
    for (y = 0; y < height; y++) for (x = 0; x < width; x++) {
        uint8_t *p = pixels + ((unsigned long)y * width + x) * 3;
        unsigned blue = !y ? 0xc9 : y >= 253 ? 0x85 :
            ((unsigned long)0x85 * y + (unsigned long)0xc9 * (255 - y)) >> 8;
        unsigned green = !y ? 0x7b : y >= 253 ? 0x3a :
            ((unsigned long)0x3a * y + (unsigned long)0x7b * (255 - y)) >> 8;
        unsigned red = !y ? 0x30 : y >= 253 ? 0xe1 :
            ((unsigned long)0xe1 * y + (unsigned long)0x30 * (255 - y)) >> 8;
        assert(p[0] == blue && p[1] == green && p[2] == red);
    }
    destroy(target); free(mask); free(pixels); free(commands);
}
static void copy_and_masks(void) {
    uint32_t colour = create(16, 12, GX_XRGB8888), coverage = create(16, 12, GX_RENDER_A8);
    uint8_t pixels[16 * 12 * 3], mask[16 * 12], expected[16 * 12 * 3];
    uint32_t commands[2 * GX_RENDER_COMMAND_WORDS];
    unsigned i;
    for (i = 0; i < sizeof(pixels); i++) pixels[i] = i * 13 & 255;
    memcpy(expected, pixels, sizeof(pixels));
    transfer(GX_RENDER_UPLOAD, colour, 16, 12, 48, pixels, sizeof(pixels));
    command(commands, GX_RENDER_COPY, 2, 3, 11, 8, 0, 0);
    commands[7] = colour; commands[8] = 0; commands[9] = 1;
    for (i = 0; i < 8; i++)
        memcpy(expected + ((unsigned long)(i + 3) * 16 + 2) * 3,
               pixels + (unsigned long)(i + 1) * 48, 33);
    batch(colour, commands, 1, NULL, 0);
    unchanged(colour, expected, 16, 12);
    /* COPY reads the result of earlier commands, including overlapping rows,
     * rather than a snapshot of the destination at batch entry. */
    command(commands, GX_RENDER_FILL, 1, 1, 9, 7, 0xb43a78UL, 255);
    command(commands + GX_RENDER_COMMAND_WORDS, GX_RENDER_COPY, 3, 3, 9, 7, 0, 0);
    commands[GX_RENDER_COMMAND_WORDS + 7] = colour;
    commands[GX_RENDER_COMMAND_WORDS + 8] = 1;
    commands[GX_RENDER_COMMAND_WORDS + 9] = 1;
    for (i = 0; i < 7; i++) {
        unsigned x;
        for (x = 0; x < 9; x++) {
            uint8_t *p = expected + ((unsigned long)(i + 1) * 16 + x + 1) * 3;
            p[0] = 0x78; p[1] = 0x3a; p[2] = 0xb4;
        }
    }
    memcpy(pixels, expected, sizeof(pixels));
    for (i = 0; i < 7; i++)
        memcpy(expected + ((unsigned long)(i + 3) * 16 + 3) * 3,
               pixels + ((unsigned long)(i + 1) * 16 + 1) * 3, 27);
    batch(colour, commands, 2, NULL, 0);
    unchanged(colour, expected, 16, 12);
    for (i = 0; i < sizeof(mask); i++) mask[i] = i * 17 & 255;
    transfer(GX_RENDER_UPLOAD, coverage, 16, 12, 16, mask, sizeof(mask));
    command(commands, GX_RENDER_A8_TARGET, 3, 2, 7, 6, 0x78ac31UL, 173);
    commands[7] = coverage; commands[8] = 5; commands[9] = 4;
    batch(colour, commands, 1, NULL, 0);
    for (i = 0; i < 6; i++) {
        unsigned x;
        for (x = 0; x < 7; x++) {
            uint8_t *p = expected + ((unsigned long)(i + 2) * 16 + x + 3) * 3;
            unsigned a = mask[(i + 4) * 16 + x + 5];
            p[0] = channel(p[0], 0x31, a, 173);
            p[1] = channel(p[1], 0xac, a, 173);
            p[2] = channel(p[2], 0x78, a, 173);
        }
    }
    unchanged(colour, expected, 16, 12);
    /* The first command must not apply when a later command is rejected. */
    command(commands, GX_RENDER_FILL, 0, 0, 16, 12, 0xffffffUL, 255);
    command(commands + GX_RENDER_COMMAND_WORDS, GX_RENDER_FILL, 0, 0, 17, 12, 0, 255);
    {
        struct gx_render r = request(GX_RENDER_BATCH, colour);
        r.commands = commands; r.count = 2;
        rejected(&r, EINVAL);
    }
    unchanged(colour, expected, 16, 12);
    command(commands + GX_RENDER_COMMAND_WORDS, GX_RENDER_A8_DATA, 0, 0, 2, 2, 0xffffffUL, 0);
    commands[GX_RENDER_COMMAND_WORDS + 8] = 2;
    {
        struct gx_render r = request(GX_RENDER_BATCH, colour);
        r.commands = commands; r.count = 2;
        r.data = (void *)((47UL << 16) | 63999UL); r.data_length = 4;
        rejected(&r, EFAULT);
    }
    unchanged(colour, expected, 16, 12);
    destroy(coverage); destroy(colour);
}
static void raster(void) {
    uint32_t target = create(513, 257, GX_RENDER_A8), primitives[6 * 33];
    struct gx_a8_primitive shape[33];
    unsigned count = 0, i, x;
    unsigned long cells = 521UL * 257;
    uint8_t *pixels = malloc(cells), *expected = malloc(cells);
    struct gx_render r;
    assert(pixels && expected);
    memset(shape, 0, sizeof(shape));
    for (i = 0; i < 24; i++) {
        shape[count].x0 = 256; shape[count].y0 = 128;
        shape[count].x1 = (int32_t)((i * 137) % 640) - 60;
        shape[count].y1 = (int32_t)((i * 79) % 360) - 40;
        shape[count++].size = i % 8 + 1;
    }
    for (i = 0; i <= 8; i++) {
        shape[count].x0 = i * 63; shape[count].y0 = 12 + i * 27;
        shape[count].dot = 1; shape[count++].size = i;
    }
    memset(expected, 19, cells);
    for (i = 0; i < 257; i++) memset(expected + (unsigned long)i * 521, 0, 513);
    for (i = 0; i < count; i++) {
        uint32_t *p = primitives + (unsigned long)i * 6;
        p[0] = shape[i].x0; p[1] = shape[i].y0; p[2] = shape[i].x1;
        p[3] = shape[i].y1; p[4] = shape[i].dot; p[5] = shape[i].size;
        if (shape[i].dot) plot_raster_dot(expected, 513, 257, 521, shape[i].x0, shape[i].y0, shape[i].size);
        else plot_raster_line(expected, 513, 257, 521, shape[i].x0, shape[i].y0, shape[i].x1, shape[i].y1, shape[i].size);
    }
    r = request(GX_RENDER_RASTER_A8, target);
    r.flags = 1; r.commands = primitives; r.count = count;
    assert(!ioctl(graphics, GX_RENDER, &r));
    memset(pixels, 19, cells);
    transfer(GX_RENDER_READBACK, target, 513, 257, 521, pixels, cells);
    assert(!memcmp(pixels, expected, cells));
    /* A late malformed primitive must not clear an existing target. */
    primitives[(count - 1UL) * 6 + 4] = 2;
    rejected(&r, EINVAL);
    memset(pixels, 19, cells);
    transfer(GX_RENDER_READBACK, target, 513, 257, 521, pixels, cells);
    assert(!memcmp(pixels, expected, cells));
    /* Merge is max coverage and does not depend on the old guest buffers. */
    primitives[(count - 1UL) * 6 + 4] = 1; r.flags = 0;
    assert(!ioctl(graphics, GX_RENDER, &r));
    memset(pixels, 19, cells);
    transfer(GX_RENDER_READBACK, target, 513, 257, 521, pixels, cells);
    for (i = 0; i < 257; i++) for (x = 0; x < 521; x++)
        assert(pixels[(unsigned long)i * 521 + x] == expected[(unsigned long)i * 521 + x]);
    destroy(target); free(pixels); free(expected);
}
static void invalid(void) {
    uint32_t target = create(8, 8, GX_XRGB8888);
    struct gx_render good = request(GX_RENDER_READBACK, target), r;
    uint8_t pixels[193], before[193];
    good.width = good.height = 8; good.stride = 24;
    good.data = pixels; good.data_length = sizeof(pixels);
    memset(pixels, 73, sizeof(pixels)); memcpy(before, pixels, sizeof(pixels));
#define BAD(field, value, error) do { r = good; r.field = (value); rejected(&r, error); \
    assert(!memcmp(pixels, before, sizeof(pixels))); } while (0)
    BAD(version, 2, EINVAL); BAD(operation, 8, EINVAL);
    BAD(target, 0, EINVAL); BAD(target, 0xffffffffUL, EINVAL);
    BAD(width, 0, EINVAL); BAD(width, 9, EINVAL); BAD(height, 9, EINVAL);
    BAD(stride, 23, EINVAL); BAD(data_length, 191, EINVAL);
    BAD(x, 1, EINVAL); BAD(format, GX_XRGB8888, EINVAL); BAD(flags, 1, EINVAL);
    BAD(dx, 1, EINVAL); BAD(dy, 1, EINVAL); BAD(count, 1, EINVAL);
    BAD(commands, (void *)1, EINVAL);
    BAD(data, NULL, EFAULT);
    BAD(data, (void *)((47UL << 16) | 63995UL), EFAULT);
    BAD(data, (void *)(240UL << 16), EFAULT);
#undef BAD
    destroy(target); rejected(&good, EINVAL);
    r = request(GX_RENDER_CREATE, 0);
    r.width = 8; r.height = 8; r.format = GX_RGB565; rejected(&r, EINVAL);
    r.format = GX_XRGB8888; r.width = 0; rejected(&r, EINVAL);
    r.width = GX_HOSTED_MAX_WIDTH * 2 + 1; rejected(&r, EINVAL);
    r.width = 8; r.height = GX_HOSTED_MAX_HEIGHT * 2 + 1; rejected(&r, EINVAL);
    /* A newly allocated target cannot resurrect a stale generation handle. */
    target = create(8, 8, GX_XRGB8888); rejected(&good, EINVAL); destroy(target);
}
static void rgb565(void) {
    uint32_t previous = owner, target;
    struct gx_window window;
    struct gx_render r;
    struct gx_handle h;
    memset(&window, 0, sizeof(window)); window.version = GX_VERSION;
    window.width = 64; window.height = 48; window.format = GX_RGB565;
    strcpy(window.title, "Retained RGB565 destination");
    assert(!ioctl(graphics, GX_CREATE_HIDDEN, &window)); owner = window.window;
    target = create(64, 48, GX_XRGB8888);
    fill(target, 64, 48, 0x739bc5UL);
    r = request(GX_RENDER_BLIT, target); r.width = 64; r.height = 48;
    assert(!ioctl(graphics, GX_RENDER, &r));
    destroy(target);
    h.version = GX_VERSION; h.handle = owner;
    assert(!ioctl(graphics, GX_DESTROY, &h)); owner = previous;
}
static void lifecycle(void) {
    uint32_t target = create(64, 48, GX_XRGB8888);
    struct gx_render r = request(GX_RENDER_BLIT, target);
    struct gx_configure config;
    struct gx_handle handle;
    uint8_t *shared = malloc(64UL * 48 * 3);
    int child, status;
    assert(shared);
    fill(target, 64, 48, 0x4d8ec7UL);
    memset(shared, 93, 64UL * 48 * 3);
    child = fork(); assert(child >= 0);
    if (!child) {
        struct gx_render foreign = request(GX_RENDER_DESTROY, target);
        rejected(&foreign, EPERM);
        /* Own fresh resources can write readback into pages inherited COW. */
        struct gx_window window;
        memset(&window, 0, sizeof(window)); window.version = GX_VERSION;
        window.width = 64; window.height = 48; window.format = GX_XRGB8888;
        strcpy(window.title, "Retained renderer child");
        assert(!ioctl(graphics, GX_CREATE_HIDDEN, &window));
        owner = window.window;
        {
            uint32_t own = create(64, 48, GX_XRGB8888);
            fill(own, 64, 48, 0x785634UL);
            transfer(GX_RENDER_READBACK, own, 64, 48, 192, shared, 64UL * 48 * 3);
            assert(shared[0] == 0x34 && shared[1] == 0x56 && shared[2] == 0x78);
            destroy(own);
        }
        handle.version = GX_VERSION; handle.handle = owner;
        assert(!ioctl(graphics, GX_DESTROY, &handle));
        _exit(17);
    }
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 17);
    for (unsigned long i = 0; i < 64UL * 48 * 3; i++) assert(shared[i] == 93);
    r.width = 64; r.height = 48; assert(!ioctl(graphics, GX_RENDER, &r));
    memset(&config, 0, sizeof(config)); config.version = GX_VERSION; config.window = owner;
    config.width = 32; config.height = 24; config.flags = GX_WINDOW_HIDDEN;
    assert(!ioctl(graphics, GX_CONFIGURE, &config));
    rejected(&r, EINVAL); /* Target survives; window destination no longer fits. */
    r.width = 32; r.height = 24; assert(!ioctl(graphics, GX_RENDER, &r));
    transfer(GX_RENDER_READBACK, target, 64, 48, 192, shared, 64UL * 48 * 3);
    assert(shared[0] == 0xc7 && shared[1] == 0x8e && shared[2] == 0x4d);
    handle.version = GX_VERSION; handle.handle = owner;
    assert(!ioctl(graphics, GX_DESTROY, &handle));
    r = request(GX_RENDER_DESTROY, target); rejected(&r, EINVAL);
    free(shared);
}
int main(void) {
    struct gx_window window;
    struct gx_render probe;
    graphics = open("/dev/gfx", O_RDWR); assert(graphics >= 0);
    memset(&probe, 0, sizeof(probe)); probe.version = GX_VERSION;
    assert(!ioctl(graphics, GX_RENDER, &probe));
    probe.count = 1; rejected(&probe, EINVAL);
    memset(&window, 0, sizeof(window)); window.version = GX_VERSION;
    window.width = 64; window.height = 48; window.format = GX_XRGB8888;
    strcpy(window.title, "Retained renderer contract");
    assert(!ioctl(graphics, GX_CREATE_HIDDEN, &window)); owner = window.window;
    puts("RENDER_STAGE roundtrip"); roundtrip(GX_XRGB8888); roundtrip(GX_RENDER_A8);
    puts("RENDER_STAGE alpha"); alpha();
    puts("RENDER_STAGE copy/masks"); copy_and_masks();
    puts("RENDER_STAGE raster"); raster();
    puts("RENDER_STAGE invalid"); invalid();
    puts("RENDER_STAGE RGB565 destination"); rgb565();
    puts("RENDER_STAGE lifecycle"); lifecycle();
    assert(!close(graphics));
    puts("RENDER_CHECK passed: exact BGR/A8 banked round trips; exhaustive alpha; "
         "coverage parity; atomic invalid batches; stale handles; fork/COW; resize/teardown");
    return 0;
}
