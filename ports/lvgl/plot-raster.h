/* SPDX-License-Identifier: MIT */
#ifndef GRAPHITE_PLOT_RASTER_H
#define GRAPHITE_PLOT_RASTER_H

#include <stdint.h>

/* Add a clipped, antialiased line to a caller-owned A8 mask. Coverage is
 * combined with max(), so adjoining segments never darken their shared pixel.
 * No allocation, clearing, LVGL state or host graphics service is involved.
 *
 * Width and height are physical pixels, each 1..8192. Stride is in A8 cells
 * and must be at least width; the entire addressed span must fit uint32_t.
 * The caller owns that storage. Invalid dimensions/strokes are ignored.
 * Stroke is 1..8 physical pixels. Even strokes retain the crisp integer
 * alignment of LVGL's horizontal/vertical lines (stroke 2 covers -1 and 0).
 * Coordinates may be any int32_t value; clipping bounds the work to the mask
 * dimensions, never an off-screen segment's length or bounding-box area. */
void plot_raster_line(uint8_t *mask, uint32_t width, uint32_t height,
                      uint32_t stride, int32_t x0, int32_t y0,
                      int32_t x1, int32_t y1, unsigned stroke);

/* Add a filled circle, including a one-pixel antialias edge inside the
 * (2 * radius + 1) square. Radius 0..8 is in physical pixels. The same span,
 * clipping and max-coverage rules apply; its bounded work is O(radius^2). */
void plot_raster_dot(uint8_t *mask, uint32_t width, uint32_t height,
                     uint32_t stride, int32_t x, int32_t y, unsigned radius);

#endif
