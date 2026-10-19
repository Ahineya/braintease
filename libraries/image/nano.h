/* SPDX-License-Identifier: MIT */
#ifndef RIPPLE_IMAGE_NANO_H
#define RIPPLE_IMAGE_NANO_H
#include "image.h"
#include <nano-X.h>
#define RI_REPEAT_X 1
#define RI_REPEAT_Y 2
struct RiClip { int x0,y0,x1,y1; };
/* Nearest-neighbour scaled/repeated RGBA drawing, clipped in window coordinates.
 * The caller supplies a nonnegative clip within 1920x1080 and retains its GC.
 * Uses the FuseDE image protocol (including ALPHA) and preserves GC clipping.
 * Temporary client/server tiles are <=8192 cells each; no retained resources.
 * Returns RI_OK, RI_MEMORY, RI_CORRUPT or RI_LIMIT (also server quota/failure).
 * Link nano.c, image/decode.c and the ordinary Nano-X/FuseDE client library. */
int ri_nano_draw(const RiImage *,GR_DRAW_ID,GR_GC_ID,int x,int y,int width,int height,
                 const struct RiClip *,unsigned flags);
/* Retained RGB565 resources: <=3 objects and 65536 cells per connection.
 * Caller must clip its GC to clip, call forget BEFORE freeing/modifying image,
 * and clear BEFORE closing Nano-X. No other API retains caller pointers. */
int ri_nano_draw_cached(const RiImage *,GR_DRAW_ID,GR_GC_ID,int,int,int,int,
                        const struct RiClip *,unsigned);
void ri_nano_forget(const RiImage *);
void ri_nano_clear(void);
#endif
