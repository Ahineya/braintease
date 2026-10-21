/* SPDX-License-Identifier: MIT */
#include "plot-raster.h"
#include <string.h>

#define RASTER_LIMIT 8192UL
#define FIXED_ONE 65536L

static int valid(const uint8_t *mask, uint32_t width, uint32_t height,
                   uint32_t stride) {
    return mask && width && height && width <= RASTER_LIMIT && height <= RASTER_LIMIT &&
           stride >= width &&
           (height == 1 || stride <= (UINT32_MAX - width) / (height - 1));
}

static int clip(double direction, double distance, double *lo, double *hi) {
    double t;
    if (!direction)
        return distance >= 0;
    t = distance / direction;
    if (direction < 0) {
        if (t > *hi)
            return 0;
        if (t > *lo)
            *lo = t;
    } else {
        if (t < *lo)
            return 0;
        if (t < *hi)
            *hi = t;
    }
    return 1;
}

static void cover(uint8_t *mask, uint32_t width, uint32_t height,
                   uint32_t stride, int32_t x, int32_t y, unsigned coverage) {
    uint8_t *at;
    if (!coverage || x < 0 || y < 0 || (uint32_t)x >= width || (uint32_t)y >= height)
        return;
    at = mask + (uint32_t)y * stride + (uint32_t)x;
    if (*at < coverage)
        *at = coverage;
}

static int32_t nearest(double x) {
    return (int32_t)(x < 0 ? x - 0.5 : x + 0.5);
}

/* After clipping, the common case needs neither a checked helper call nor
 * division for every covered sample. Keep signed clipping at the boundary;
 * the inner loop only visits known in-bounds cells. */
static int draw_inside(uint8_t *mask, uint32_t width, uint32_t height, uint32_t stride,
                        int32_t start, int32_t end, int32_t fixed, int32_t step,
                        unsigned steep, unsigned stroke) {
    int32_t finish = fixed + (end - start) * step;
    int32_t low = (fixed < finish ? fixed : finish) / FIXED_ONE - (int32_t)stroke / 2;
    int32_t high = (fixed > finish ? fixed : finish) / FIXED_ONE - (int32_t)stroke / 2;
    uint32_t major_limit = steep ? height : width, minor_limit = steep ? width : height;
    uint32_t position;
    int32_t major;
    if (start < 0 || end < start || (uint32_t)end >= major_limit || fixed < 0 || finish < 0 ||
        low < 0 || (uint32_t)high + stroke >= minor_limit)
        return 0;
    position = (uint32_t)fixed;
    if (!step && !(position & 65535UL)) {
        unsigned n;
        if (!steep) {
            uint8_t *at = mask + (uint32_t)low * stride + (uint32_t)start;
            for (n = 0; n < stroke; n++, at += stride)
                memset(at, 255, (uint32_t)(end - start + 1));
        } else {
            uint8_t *at = mask + (uint32_t)start * stride + (uint32_t)low;
            for (major = start; major <= end; major++, at += stride)
                for (n = 0; n < stroke; n++)
                    at[n] = 255;
        }
        return 1;
    }
    for (major = start; major <= end; major++) {
        uint32_t base = (position >> 16) - stroke / 2;
        unsigned upper = (unsigned)(((position & 65535UL) * 255UL + 32768UL) >> 16);
        uint8_t *at = steep ? mask + (uint32_t)major * stride + base
                             : mask + base * stride + (uint32_t)major;
        uint32_t advance = steep ? 1 : stride;
        unsigned n;
        if (*at < 255 - upper)
            *at = 255 - upper;
        for (n = 1; n < stroke; n++) {
            at += advance;
            *at = 255;
        }
        at += advance;
        if (*at < upper)
            *at = upper;
        position += (uint32_t)step;
    }
    return 1;
}

void plot_raster_line(uint8_t *mask, uint32_t width, uint32_t height,
                      uint32_t stride, int32_t x0, int32_t y0,
                      int32_t x1, int32_t y1, unsigned stroke) {
    double ax = x0, ay = y0, dx = (double)x1 - x0, dy = (double)y1 - y0;
    double lo = 0, hi = 1, halo, bx, by, first, last, minor, slope;
    int32_t start, end, fixed, step, major;
    unsigned steep, n;
    if (!valid(mask, width, height, stride) || !stroke || stroke > 8)
        return;
    if (!dx && !dy) {
        int32_t offset = (int32_t)stroke / 2;
        /* Compare in double before subtracting the small stroke offset: even
         * a zero-length INT32_MIN point must not overflow an integer. */
        if (ax < -(double)stroke || ay < -(double)stroke ||
            ax > (double)width + stroke || ay > (double)height + stroke)
            return;
        for (n = 0; n < stroke; n++) {
            unsigned m;
            for (m = 0; m < stroke; m++)
                cover(mask, width, height, stride, x0 - offset + (int32_t)m,
                       y0 - offset + (int32_t)n, 255);
        }
        return;
    }
    steep = (dy < 0 ? -dy : dy) > (dx < 0 ? -dx : dx);
    /* Clip once, in floating point so subtracting two extreme int32_t inputs
     * is safe. Only the bounded on-screen span enters the integer loop. The
     * halo keeps antialias coverage from just-outside centerlines visible. */
    halo = (double)stroke + 1;
    if (!clip(-dx, ax + halo, &lo, &hi) ||
        !clip(dx, (double)width - 1 + halo - ax, &lo, &hi) ||
        !clip(-dy, ay + halo, &lo, &hi) ||
        !clip(dy, (double)height - 1 + halo - ay, &lo, &hi))
        return;
    bx = ax + hi * dx;
    by = ay + hi * dy;
    ax += lo * dx;
    ay += lo * dy;
    if ((steep ? ay : ax) > (steep ? by : bx)) {
        double swap = ax;
        ax = bx;
        bx = swap;
        swap = ay;
        ay = by;
        by = swap;
    }
    first = steep ? ay : ax;
    last = steep ? by : bx;
    start = (int32_t)first;
    if ((double)start < first)
        start++;
    end = (int32_t)last;
    if ((double)end > last)
        end--;
    slope = steep ? dx / dy : dy / dx;
    minor = (steep ? ax : ay) + ((double)start - first) * slope;
    fixed = nearest(minor * FIXED_ONE);
    step = nearest(slope * FIXED_ONE);
    if (draw_inside(mask, width, height, stride, start, end, fixed, step, steep, stroke))
        return;
    for (major = start; major <= end; major++) {
        int32_t base = fixed / FIXED_ONE, fraction = fixed % FIXED_ONE, low;
        unsigned upper;
        if (fraction < 0) {
            base--;
            fraction += FIXED_ONE;
        }
        low = base - (int32_t)stroke / 2;
        upper = (unsigned)((fraction * 255L + 32768L) / FIXED_ONE);
        if (steep)
            cover(mask, width, height, stride, low, major, 255 - upper);
        else
            cover(mask, width, height, stride, major, low, 255 - upper);
        for (n = 1; n < stroke; n++) {
            if (steep)
                cover(mask, width, height, stride, low + (int32_t)n, major, 255);
            else
                cover(mask, width, height, stride, major, low + (int32_t)n, 255);
        }
        if (steep)
            cover(mask, width, height, stride, low + (int32_t)stroke, major, upper);
        else
            cover(mask, width, height, stride, major, low + (int32_t)stroke, upper);
        fixed += step;
    }
}

static uint32_t square_root(uint32_t value) {
    uint32_t root = 0, bit = 1UL << 22;
    while (bit > value)
        bit >>= 2;
    while (bit) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else
            root >>= 1;
        bit >>= 2;
    }
    return root;
}

void plot_raster_dot(uint8_t *mask, uint32_t width, uint32_t height,
                     uint32_t stride, int32_t x, int32_t y, unsigned radius) {
    int32_t dx, dy, r = (int32_t)radius;
    uint32_t inner, outer;
    if (!valid(mask, width, height, stride) || radius > 8 ||
        x < -r || y < -r || x > (int32_t)(width - 1) + r ||
        y > (int32_t)(height - 1) + r)
        return;
    inner = (uint32_t)r * r;
    outer = (uint32_t)(r + 1) * (r + 1);
    /* Distances are evaluated only in a quadrant; four mirrored pixels share
     * one integer result. An 8.8 integer square root is needed just on the
     * antialias rim, with no floating point inside the pixel loop. */
    for (dy = 0; dy <= r; dy++)
        for (dx = 0; dx <= r; dx++) {
            uint32_t distance = (uint32_t)dx * dx + (uint32_t)dy * dy;
            unsigned coverage;
            if (distance <= inner)
                coverage = 255;
            else if (distance >= outer)
                continue;
            else {
                uint32_t edge = (uint32_t)(r + 1) * 256UL - square_root(distance << 16);
                coverage = (unsigned)((edge * 255UL + 128) / 256UL);
            }
            cover(mask, width, height, stride, x + dx, y + dy, coverage);
            if (dx)
                cover(mask, width, height, stride, x - dx, y + dy, coverage);
            if (dy) {
                cover(mask, width, height, stride, x + dx, y - dy, coverage);
                if (dx)
                    cover(mask, width, height, stride, x - dx, y - dy, coverage);
            }
        }
}
