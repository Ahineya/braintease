/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef FDE_SKIN_H
#define FDE_SKIN_H

/* Toolkit-independent geometry. All dimensions count logical pixels, not C
 * cells or file octets. The centre flag controls painting, not content layout.
 * A skin needs a nonempty centre in its source image. Borders never shrink. */
struct fde_skin {
    unsigned width, height;
    unsigned left, top, right, bottom;
    unsigned centre;
};

struct fde_patch {
    unsigned sx, sy, sw, sh;
    unsigned dx, dy, dw, dh;
};
struct fde_asset {unsigned x, y; struct fde_skin skin;};

/* Return patch count (0..9), or -1 for invalid metadata/undersized destination.
 * Output is unchanged on failure. Zero-area patches are omitted. Coordinates
 * are local to the image/destination; a backend adds atlas and window origins,
 * clips destination painting, and uses nearest-neighbour sampling. Source and
 * destination dimensions are bounded to 32767 for the signed C16 drawing API.
 * This routine does not allocate, draw, flush, access files or manage windows. */
int fde_slice(const struct fde_skin *skin, unsigned width, unsigned height,
              struct fde_patch patches[9]);
#endif
