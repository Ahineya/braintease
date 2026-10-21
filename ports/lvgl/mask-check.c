/* SPDX-License-Identifier: MIT */
/* Real GX ioctl validation and exact parity with the portable A8 renderer. */
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
#include <unistd.h>

static void cpu(uint8_t *mask, unsigned width, unsigned height, unsigned stride,
                 const struct gx_a8_primitive *p, unsigned long count) {
    unsigned long i;
    for (i = 0; i < count; i++) {
        if (p[i].dot)
            plot_raster_dot(mask, width, height, stride, p[i].x0, p[i].y0, p[i].size);
        else
            plot_raster_line(mask, width, height, stride, p[i].x0, p[i].y0,
                             p[i].x1, p[i].y1, p[i].size);
    }
}
static void rejected(int fd, struct gx_raster_a8 *r, unsigned error,
                      const uint8_t *actual, const uint8_t *expected, unsigned long count) {
    errno = 0;
    assert(ioctl(fd, GX_RASTER_A8, r) < 0 && errno == error);
    assert(!memcmp(actual, expected, count));
}
static void parity(int fd, unsigned width, unsigned height, unsigned stride,
                    const struct gx_a8_primitive *list, unsigned long count) {
    unsigned long cells = (unsigned long)height * stride, i;
    unsigned y;
    uint8_t *actual = malloc(cells + 2), *expected = malloc(cells + 2);
    struct gx_raster_a8 r;
    assert(actual && expected);
    memset(actual, 93, cells + 2);
    memcpy(expected, actual, cells + 2);
    for (y = 0; y < height; y++)
        memset(expected + 1 + (unsigned long)y * stride, 0, width);
    cpu(expected + 1, width, height, stride, list, count);
    memset(&r, 0, sizeof(r));
    r.version = GX_VERSION;
    r.width = width;
    r.height = height;
    r.stride = stride;
    r.flags = GX_A8_CLEAR;
    r.count = count;
    r.dst = actual + 1;
    r.primitives = list;
    assert(!ioctl(fd, GX_RASTER_A8, &r));
    for (i = 0; i < cells + 2; i++) {
        if (actual[i] != expected[i])
            fprintf(stderr, "MASK_MISMATCH at=%lu actual=%u expected=%u\n",
                    i, (unsigned)actual[i], (unsigned)expected[i]);
        assert(actual[i] == expected[i]);
    }
    /* Retain existing coverage when CLEAR is absent; padding and guards must
     * survive both modes, including destination spans across C16 banks. */
    for (y = 0; y < height; y++)
        memset(actual + 1 + (unsigned long)y * stride, 12, width);
    memcpy(expected, actual, cells + 2);
    cpu(expected + 1, width, height, stride, list, count);
    r.flags = 0;
    assert(!ioctl(fd, GX_RASTER_A8, &r));
    assert(!memcmp(actual, expected, cells + 2));
    free(actual);
    free(expected);
}
int main(void) {
    struct gx_raster_a8 r, good;
    struct gx_a8_primitive shapes[64], invalid[2];
    static const int32_t ends[][2] = {
        {65,40}, {50,54}, {20,54}, {4,40}, {4,8}, {20,3}, {50,3}, {65,8}
    };
    uint8_t original[128], actual[128];
    unsigned count = 0, i, stroke;
    int fd = open("/dev/gfx", O_RDWR);
    assert(fd >= 0);
    assert(sizeof(struct gx_a8_primitive) == 10 && sizeof(struct gx_raster_a8) == 11);
    memset(&r, 0, sizeof(r));
    r.version = GX_VERSION;
    assert(!ioctl(fd, GX_RASTER_A8, &r));
    for (stroke = 1; stroke <= 2; stroke++)
        for (i = 0; i < 8; i++) {
            struct gx_a8_primitive *p = &shapes[count++];
            p->x0 = 35; p->y0 = 28; p->x1 = ends[i][0]; p->y1 = ends[i][1];
            p->dot = 0; p->size = stroke;
        }
    for (i = 0; i <= 8; i++) {
        struct gx_a8_primitive *p = &shapes[count++];
        p->x0 = i * 8; p->y0 = 18; p->x1 = p->y1 = 0;
        p->dot = 1; p->size = i;
    }
    shapes[count].x0 = INT32_MIN; shapes[count].y0 = INT32_MIN;
    shapes[count].x1 = INT32_MAX; shapes[count].y1 = INT32_MAX;
    shapes[count].dot = 0; shapes[count++].size = 2;
    shapes[count].x0 = INT32_MIN; shapes[count].y0 = 10;
    shapes[count].x1 = INT32_MAX; shapes[count].y1 = 10;
    shapes[count].dot = 0; shapes[count++].size = 1;
    shapes[count].x0 = 10; shapes[count].y0 = INT32_MIN;
    shapes[count].x1 = 10; shapes[count].y1 = INT32_MAX;
    shapes[count].dot = 0; shapes[count++].size = 2;
    shapes[count].x0 = -10000; shapes[count].y0 = -4000;
    shapes[count].x1 = 10000; shapes[count].y1 = 4000;
    shapes[count].dot = 0; shapes[count++].size = 1;
    parity(fd, 73, 59, 81, shapes, count);
    parity(fd, 513, 257, 521, shapes, count);
    parity(fd, 1026, 514, 1033, shapes, count);
    /* A list straddling a bank boundary exercises the full borrowed span. */
    {
        struct gx_a8_primitive *many = calloc(8192UL, sizeof(*many));
        unsigned long at;
        assert(many);
        for (at = 0; at < 8192UL; at++) {
            many[at].x0 = many[at].y0 = 3;
            many[at].dot = 1;
        }
        parity(fd, 8, 8, 11, many, 8192UL);
        free(many);
    }
    memset(actual, 73, sizeof(actual));
    memcpy(original, actual, sizeof(actual));
    memset(&good, 0, sizeof(good));
    good.version = GX_VERSION;
    good.width = 8; good.height = 8; good.stride = 9; good.flags = GX_A8_CLEAR;
    good.dst = actual + 1; good.primitives = shapes; good.count = 1;
#define BAD(field, value, error) do { r = good; r.field = (value); \
    rejected(fd, &r, (error), actual, original, sizeof(actual)); } while (0)
    BAD(version, 2, EINVAL);
    BAD(width, 0, EINVAL);
    BAD(width, GX_HOSTED_MAX_WIDTH * 2 + 1, EINVAL);
    BAD(height, GX_HOSTED_MAX_HEIGHT * 2 + 1, EINVAL);
    BAD(stride, 7, EINVAL);
    BAD(flags, 2, EINVAL);
    BAD(count, 8193UL, EINVAL);
    BAD(dst, NULL, EFAULT);
    BAD(primitives, NULL, EFAULT);
    BAD(primitives, (void *)(240UL << 16), EFAULT);
    BAD(primitives, (void *)((47UL << 16) | 63995UL), EFAULT);
    BAD(primitives, (void *)(actual + 1), EINVAL);
    BAD(dst, (void *)((47UL << 16) | 63995UL), EFAULT);
    BAD(dst, (void *)((256UL << 16) | 65535UL), EFAULT);
    invalid[0] = shapes[0]; invalid[1] = shapes[0];
    r = good; r.primitives = invalid; r.count = 2;
    invalid[1].dot = 2;
    rejected(fd, &r, EINVAL, actual, original, sizeof(actual));
    invalid[1].dot = 0; invalid[1].size = 0;
    rejected(fd, &r, EINVAL, actual, original, sizeof(actual));
    invalid[1].size = 9;
    rejected(fd, &r, EINVAL, actual, original, sizeof(actual));
    invalid[1].dot = 1;
    rejected(fd, &r, EINVAL, actual, original, sizeof(actual));
    /* A valid empty list can clear, or preserve, a mask on its own. */
    good.count = 0; good.primitives = NULL;
    assert(!ioctl(fd, GX_RASTER_A8, &good));
    for (i = 0; i < 8; i++) {
        unsigned x;
        for (x = 0; x < 8; x++)
            assert(!actual[1 + i * 9 + x]);
    }
    assert(actual[0] == 73 && actual[9] == 73 && actual[73] == 73);
    assert(!close(fd));
    puts("MASK_CHECK passed: exact CPU/GX parity; 1x/2x AA; clipped extremes; "
         "banked buffers/lists; preserved padding; transactional parameter/span/overlap errors");
    return 0;
}
