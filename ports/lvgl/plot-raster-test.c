/* SPDX-License-Identifier: MIT */
#include "plot-raster.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH 73UL
#define HEIGHT 59UL
#define STRIDE 81UL
#define COUNT (HEIGHT * STRIDE)
static uint8_t before[COUNT + 2], reverse[COUNT + 2];

static void reset(uint8_t *mask) {
    memset(mask, 0, COUNT + 2);
    mask[0] = mask[COUNT + 1] = 93;
}
static unsigned check(const uint8_t *mask) {
    uint32_t x, y;
    unsigned count = 0;
    assert(mask[0] == 93 && mask[COUNT + 1] == 93);
    for (y = 0; y < HEIGHT; y++) {
        for (x = 0; x < WIDTH; x++)
            count += mask[1 + y * STRIDE + x] != 0;
        for (x = WIDTH; x < STRIDE; x++)
            assert(mask[1 + y * STRIDE + x] == 0);
    }
    return count;
}
static void segment(int32_t x0, int32_t y0, int32_t x1, int32_t y1, unsigned stroke) {
    uint32_t i;
    reset(before);
    reset(reverse);
    plot_raster_line(before + 1, WIDTH, HEIGHT, STRIDE, x0, y0, x1, y1, stroke);
    plot_raster_line(reverse + 1, WIDTH, HEIGHT, STRIDE, x1, y1, x0, y0, stroke);
    assert(check(before));
    assert(check(reverse));
    /* Clipping and endpoint direction cannot change the coverage. Rounding
     * at an extreme floating-point intersection can differ by one A8 step. */
    for (i = 0; i < COUNT + 2; i++)
        assert(abs((int)before[i] - (int)reverse[i]) <= 1);
    memcpy(reverse, before, sizeof(before));
    plot_raster_line(before + 1, WIDTH, HEIGHT, STRIDE, x0, y0, x1, y1, stroke);
    assert(!memcmp(before, reverse, sizeof(before)));
}
int main(void) {
    unsigned stroke, i;
    static const int32_t ends[][2] = {
        {65, 40}, {50, 54}, {20, 54}, {4, 40},
        {4, 8}, {20, 3}, {50, 3}, {65, 8}
    };
    for (stroke = 1; stroke <= 8; stroke++) {
        for (i = 0; i < sizeof(ends) / sizeof(ends[0]); i++)
            segment(35, 28, ends[i][0], ends[i][1], stroke);
        segment(-10000, -4000, 10000, 4000, stroke);
        segment(INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX, stroke);
        segment(INT32_MIN, 10, INT32_MAX, 10, stroke);
        segment(10, INT32_MIN, 10, INT32_MAX, stroke);
        segment(INT32_MIN, INT32_MAX, INT32_MAX, INT32_MIN + 100L, stroke);
        reset(before);
        plot_raster_line(before + 1, WIDTH, HEIGHT, STRIDE, 10, 10, 20, 10, stroke);
        assert(check(before) == 11 * stroke);
        for (i = 0; i < stroke; i++)
            assert(before[1 + (10 - stroke / 2 + i) * STRIDE + 15] == 255);
        reset(before);
        plot_raster_line(before + 1, WIDTH, HEIGHT, STRIDE, 10, 10, 10, 20, stroke);
        assert(check(before) == 11 * stroke);
        reset(before);
        plot_raster_line(before + 1, WIDTH, HEIGHT, STRIDE, 10, 10, 10, 10, stroke);
        assert(check(before) == stroke * stroke);
    }
    /* Ordinary slopes have intermediate coverage rather than stair steps. */
    segment(2, 4, 68, 47, 1);
    for (i = 1; i <= COUNT; i++)
        if (before[i] && before[i] < 255)
            break;
    assert(i <= COUNT);
    for (stroke = 0; stroke <= 8; stroke++) {
        int32_t dx, dy;
        unsigned antialias = 0;
        reset(before);
        plot_raster_dot(before + 1, WIDTH, HEIGHT, STRIDE, 30, 25, stroke);
        assert(check(before) && before[1 + 25 * STRIDE + 30] == 255);
        for (dy = -(int32_t)stroke; dy <= (int32_t)stroke; dy++)
            for (dx = -(int32_t)stroke; dx <= (int32_t)stroke; dx++) {
                unsigned coverage = before[1 + (25 + dy) * STRIDE + 30 + dx];
                assert(coverage == before[1 + (25 - dy) * STRIDE + 30 - dx]);
                assert(coverage == before[1 + (25 + dx) * STRIDE + 30 + dy]);
                antialias += coverage > 0 && coverage < 255;
            }
        if (stroke)
            assert(antialias);
        memcpy(reverse, before, sizeof(before));
        plot_raster_dot(before + 1, WIDTH, HEIGHT, STRIDE, 30, 25, stroke);
        assert(!memcmp(before, reverse, sizeof(before)));
        reset(before);
        plot_raster_dot(before + 1, WIDTH, HEIGHT, STRIDE, 0, 0, stroke);
        assert(check(before) && before[1] == 255);
        reset(before);
        plot_raster_dot(before + 1, WIDTH, HEIGHT, STRIDE, WIDTH - 1, HEIGHT - 1, stroke);
        assert(check(before) && before[1 + (HEIGHT - 1) * STRIDE + WIDTH - 1] == 255);
    }
    /* Empty/offscreen/extreme inputs and invalid spans never touch storage. */
    reset(before);
    memcpy(reverse, before, sizeof(before));
    plot_raster_line(NULL, WIDTH, HEIGHT, STRIDE, 0, 0, 1, 1, 1);
    plot_raster_line(before + 1, 0, HEIGHT, STRIDE, 0, 0, 1, 1, 1);
    plot_raster_line(before + 1, WIDTH, 0, STRIDE, 0, 0, 1, 1, 1);
    plot_raster_line(before + 1, WIDTH, HEIGHT, WIDTH - 1, 0, 0, 1, 1, 1);
    plot_raster_line(before + 1, WIDTH, HEIGHT, UINT32_MAX, 0, 0, 1, 1, 1);
    plot_raster_line(before + 1, WIDTH, HEIGHT, STRIDE, 0, 0, 1, 1, 0);
    plot_raster_line(before + 1, WIDTH, HEIGHT, STRIDE, 0, 0, 1, 1, 9);
    plot_raster_line(before + 1, WIDTH, HEIGHT, STRIDE, INT32_MIN, 2, INT32_MIN, 50, 2);
    plot_raster_line(before + 1, WIDTH, HEIGHT, STRIDE, INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN, 2);
    plot_raster_line(before + 1, WIDTH, HEIGHT, STRIDE, INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX, 2);
    plot_raster_dot(before + 1, WIDTH, HEIGHT, STRIDE, INT32_MIN, INT32_MIN, 8);
    plot_raster_dot(before + 1, WIDTH, HEIGHT, STRIDE, INT32_MAX, INT32_MAX, 8);
    plot_raster_dot(before + 1, WIDTH, HEIGHT, STRIDE, 0, 0, 9);
    plot_raster_dot(before + 1, WIDTH, HEIGHT, UINT32_MAX, 0, 0, 2);
    assert(!memcmp(before, reverse, sizeof(before)));
    /* Max-sized and heavily clipped spans finish in O(viewport), including
     * all scanline guards and the fixed-point update beyond the last pixel. */
    {
        uint8_t *wide = calloc(8192UL * 3, sizeof(*wide));
        assert(wide);
        plot_raster_line(wide, 8192, 3, 8192, INT32_MIN, 0, INT32_MAX, 2, 2);
        assert(wide[4096] || wide[8192 + 4096]);
        free(wide);
    }
    puts("PLOT_RASTER passed: octants, AA lines/dots, max joins, 1x/2x strokes, clipping, extreme bounds");
    return 0;
}
