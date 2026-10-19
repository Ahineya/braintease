/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef FDE_NANO_H
#define FDE_NANO_H
#include "nano-X.h"
#include "fde_skin.h"
#include "fde_raster.h"
typedef unsigned long FDE_IMAGE;
/* Status values: 0 success, 1 invalid, 2 quota, 3 allocation, 4 incomplete, 5 file I/O.
 * All resources belong to the Nano-X connection and die on disconnect.
 * Creation returns zero on failure; use fde_image_status() for the status.
 * MASK_ONLY resources have no colour pixels; drawing uses the GC foreground. */
#define FDE_IMAGE_MASK 1
#define FDE_IMAGE_MASK_ONLY 2
/* RGB565 pixels followed by one 0..255 alpha cell per pixel, same row order.
 * ALPHA cannot be combined with MASK/MASK_ONLY; existing quotas still apply. */
#define FDE_IMAGE_ALPHA 4
#define FDE_IMAGE_TINT 1
unsigned fde_image_status(void);
FDE_IMAGE fde_image_create(unsigned, unsigned, unsigned);
int fde_image_upload(FDE_IMAGE, unsigned long, const unsigned short *, unsigned);
int fde_image_destroy(FDE_IMAGE);
int fde_image_draw(FDE_IMAGE, GR_DRAW_ID, GR_GC_ID, const struct fde_rect *, unsigned, unsigned);
/* Upload one complete immutable resource, copying caller data synchronously.
 * pixels == NULL with mask != NULL creates a compact tint-only resource. */
FDE_IMAGE fde_image_load(unsigned, unsigned, const unsigned short *, const unsigned short *);
FDE_IMAGE fde_image_load_file(const char *);
int fde_skin_paint(FDE_IMAGE, GR_DRAW_ID, GR_GC_ID, unsigned, unsigned,
                   const struct fde_skin *, int, int, unsigned, unsigned);
#endif
