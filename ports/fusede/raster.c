/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "include/fde_raster.h"

int fde_raster_valid(const struct fde_pixels *s, const struct fde_rect *r)
{
    return s && r && (s->pixels || s->mask) && s->width && s->height &&
        s->width <= 512 && s->height <= 512 && r->sw && r->sh &&
        r->width && r->height && r->width <= 1920 && r->height <= 1080 &&
        (unsigned long)r->sx + r->sw <= s->width &&
        (unsigned long)r->sy + r->sh <= s->height;
}

int fde_raster(const struct fde_pixels *s, const struct fde_rect *r,
               unsigned cw, unsigned ch, unsigned tinted, unsigned short tint,
               fde_span_fn span, void *context)
{
    long right, bottom;
    int left, top, x, y, end, run;
    unsigned sx, sy, acc, mpitch, colour = 0, next = 0, painted, active;
    unsigned long row, maskrow;
    if (!fde_raster_valid(s, r) || !span || !cw || !ch || cw > 1920 || ch > 1080)
        return -1;
    right = (long)r->x + r->width; bottom = (long)r->y + r->height;
    if (right > cw) right = cw; if (bottom > ch) bottom = ch;
    left = r->x < 0 ? 0 : r->x; top = r->y < 0 ? 0 : r->y;
    if (right <= left || bottom <= top) return 0;
    mpitch = (s->width + 15) / 16;
    for (y = top; y < bottom; ++y) {
        sy = r->sy + (unsigned long)(y - (long)r->y) * r->sh / r->height;
        row = (unsigned long)sy * s->width; maskrow = (unsigned long)sy * mpitch;
        /* Nine-slice centres and edges frequently stretch one source column
         * across hundreds of pixels. Sample it once, emitting the whole span. */
        if (r->sw == 1 || (tinted && !s->mask)) {
            sx = r->sx;
            if (!s->mask || (s->mask[maskrow + sx / 16] & (0x8000U >> (sx % 16))))
                span(context, left, y, (int)right - left,
                     tinted || !s->pixels ? tint : s->pixels[row + sx]);
            continue;
        }
        /* Theme interiors usually contain a flat horizontal gradient row.
         * When stretching, scanning the small source row once avoids sampling
         * and comparing the same colour at every destination pixel. */
        if (r->width >= r->sw * 2) {
            sx = r->sx;
            active = !s->mask || (s->mask[maskrow + sx / 16] & (0x8000U >> (sx % 16))) != 0;
            if (active) colour = tinted || !s->pixels ? tint : s->pixels[row + sx];
            for (acc = 1; acc < r->sw; ++acc) {
                ++sx;
                painted = !s->mask || (s->mask[maskrow + sx / 16] & (0x8000U >> (sx % 16))) != 0;
                if (painted != active || (painted && (tinted || !s->pixels ? tint : s->pixels[row + sx]) != colour)) break;
            }
            if (acc == r->sw) {
                if (active) span(context, left, y, (int)right - left, colour);
                continue;
            }
        }
        sx = r->sx + (unsigned long)(left - (long)r->x) * r->sw / r->width;
        acc = (unsigned long)(left - (long)r->x) * r->sw % r->width;
        active = 0; run = left; end = (int)right;
        for (x = left; x < end; ++x) {
            painted = !s->mask || (s->mask[maskrow + sx / 16] & (0x8000U >> (sx % 16)));
            if (painted) next = tinted || !s->pixels ? tint : s->pixels[row + sx];
            if (active && (!painted || next != colour)) {
                span(context, run, y, x - run, colour); active = 0;
            }
            if (painted && !active) {run = x; colour = next; active = 1;}
            acc += r->sw;
            while (acc >= r->width) {acc -= r->width; ++sx;}
        }
        if (active) span(context, run, y, end - run, colour);
    }
    return 0;
}
