/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef FDE_RASTER_H
#define FDE_RASTER_H
struct fde_pixels {
    const unsigned short *pixels, *mask;
    unsigned width, height;
};
struct fde_rect {
    int x, y;
    unsigned sx, sy, sw, sh, width, height;
};
typedef void (*fde_span_fn)(void *, int, int, unsigned, unsigned short);
/* Nearest-neighbour sampling preserves phase after clipping to the destination
 * bounds. Mask words count 16 pixels, MSB first, with padded rows. NULL pixels
 * means a tint-only mask. The caller validates buffer extents/lifetime and
 * guarantees disjoint source/destination. No allocation or global state.
 * Validates the whole operation before emitting any spans. */
int fde_raster_valid(const struct fde_pixels *, const struct fde_rect *);
int fde_raster(const struct fde_pixels *, const struct fde_rect *,
               unsigned, unsigned, unsigned, unsigned short, fde_span_fn, void *);
#endif
