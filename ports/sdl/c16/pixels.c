/*
 * C16 software pixel access for SDL 1.2.
 *
 * SDL surface memory is an array of logical octets, even when a C char has
 * sixteen bits. Never cast the storage to Uint16* or Uint32*: doing so changes
 * both its stride and its representation on that ABI. Keep this adaptation
 * below the SDL surface API so GX and future byte-addressed ports can share
 * the ordinary SDL ownership, clipping and format contracts.
 */
#include "SDL_config.h"
#include "SDL_error.h"
#include "SDL_endian.h"
#include "pixels.h"

Uint32 SDL_C16LoadPixel(const Uint8 *pixels, int octets)
{
    Uint32 pixel = 0;
#if SDL_BYTEORDER == SDL_LIL_ENDIAN
    switch (octets) {
        case 4: pixel |= (Uint32)(pixels[3] & 255) << 24; /* fall through */
        case 3: pixel |= (Uint32)(pixels[2] & 255) << 16; /* fall through */
        case 2: pixel |= (Uint32)(pixels[1] & 255) << 8;  /* fall through */
        case 1: pixel |= (Uint32)(pixels[0] & 255); break;
    }
#else
    int i;
    for (i = 0; i < octets; ++i) {
        pixel = (pixel << 8) | (Uint32)(pixels[i] & 255);
    }
#endif
    return pixel;
}

void SDL_C16StorePixel(Uint8 *pixels, int octets, Uint32 pixel)
{
#if SDL_BYTEORDER == SDL_LIL_ENDIAN
    switch (octets) {
        case 4: pixels[3] = (Uint8)((pixel >> 24) & 255); /* fall through */
        case 3: pixels[2] = (Uint8)((pixel >> 16) & 255); /* fall through */
        case 2: pixels[1] = (Uint8)((pixel >> 8) & 255);  /* fall through */
        case 1: pixels[0] = (Uint8)(pixel & 255); break;
    }
#else
    int i;
    for (i = 0; i < octets; ++i) {
        pixels[octets - 1 - i] = (Uint8)((pixel >> (i * 8)) & 255);
    }
#endif
}

static unsigned channel(Uint32 pixel, Uint32 mask, int shift, int loss)
{
    if (!mask) return 0;
    return (unsigned)(((pixel & mask) >> shift) << loss);
}

static void unpack(Uint32 pixel, const SDL_PixelFormat *format,
                   unsigned *r, unsigned *g, unsigned *b)
{
    if (format->palette) {
        const SDL_Color *color = &format->palette->colors[pixel & 255];
        *r = color->r & 255;
        *g = color->g & 255;
        *b = color->b & 255;
    } else {
        /* SDL's software blitters use left-aligned channels rather than the
         * full-range replication performed by SDL_GetRGB. */
        *r = channel(pixel, format->Rmask, format->Rshift, format->Rloss);
        *g = channel(pixel, format->Gmask, format->Gshift, format->Gloss);
        *b = channel(pixel, format->Bmask, format->Bshift, format->Bloss);
    }
}

static Uint32 pack(const SDL_PixelFormat *format, unsigned r, unsigned g,
                   unsigned b, unsigned a)
{
    return (((Uint32)r >> format->Rloss) << format->Rshift & format->Rmask)
         | (((Uint32)g >> format->Gloss) << format->Gshift & format->Gmask)
         | (((Uint32)b >> format->Bloss) << format->Bshift & format->Bmask)
         | (((Uint32)a >> format->Aloss) << format->Ashift & format->Amask);
}

/* Match the generic SDL 1.2 ALPHA_BLEND rounding, using a wide signed
 * intermediate so a 16-bit int cannot overflow. The numerator is nonnegative
 * and division therefore does not depend on signed right-shift behavior. */
static unsigned blend(unsigned source, unsigned destination, unsigned alpha)
{
    return (unsigned)(((Sint32)source * (Sint32)alpha
        + (Sint32)destination * (Sint32)(256 - alpha) + 255) / 256);
}

/* The RGB565/RGB555 identity blitters deliberately use five-bit alpha. */
static Uint32 blend16(Uint32 source, Uint32 destination,
                      const SDL_PixelFormat *format, unsigned alpha, int half)
{
    Uint32 mask = format->Gmask == 0x07e0UL ? 0x07e0f81fUL : 0x03e07c1fUL;
    Uint32 lowmask = format->Gmask == 0x07e0UL ? 0xf7deUL : 0xfbdeUL;
    if (half && alpha == 128) {
        return ((source & lowmask) >> 1) + ((destination & lowmask) >> 1)
             + (source & destination & ~lowmask);
    }
    source = (source | (source << 16)) & mask;
    destination = (destination | (destination << 16)) & mask;
    destination += ((source - destination) * (alpha >> 3)) >> 5;
    destination &= mask;
    return (destination | (destination >> 16)) & 65535UL;
}

static int rgb888(const SDL_PixelFormat *format)
{
    return format->BytesPerPixel == 4
        && format->Rmask == 0x00ff0000UL
        && format->Gmask == 0x0000ff00UL
        && format->Bmask == 0x000000ffUL;
}

static int rgb16(const SDL_PixelFormat *format)
{
    return format->BytesPerPixel == 2 && !format->Amask
        && format->Bmask == 0x001fUL
        && ((format->Rmask == 0xf800UL && format->Gmask == 0x07e0UL)
         || (format->Rmask == 0x7c00UL && format->Gmask == 0x03e0UL));
}

static void copy_pixels(SDL_BlitInfo *info, SDL_Surface *source)
{
    Uint8 *src = info->s_pixels;
    Uint8 *dst = info->d_pixels;
    long row_octets = (long)info->d_width * info->src->BytesPerPixel;
    long src_pitch = row_octets + info->s_skip;
    long dst_pitch = row_octets + info->d_skip;
    int height = info->d_height;
    int same_surface = source == source->map->dst;
    int disjoint = !same_surface && !(source->flags & SDL_PREALLOC)
        && !(source->map->dst->flags & SDL_PREALLOC);
    if (height <= 0 || row_octets <= 0) return;
    if (same_surface) {
        if (src == dst && src_pitch == dst_pitch) return;
        disjoint = dst > src ? dst - src >= row_octets : src - dst >= row_octets;
    }
    /* memmove alone is insufficient when a downward copy overlaps later
     * source rows. Compare pointers only inside the same surface allocation. */
    if (same_surface && dst > src) {
        src += (long)(height - 1) * src_pitch;
        dst += (long)(height - 1) * dst_pitch;
        src_pitch = -src_pitch;
        dst_pitch = -dst_pitch;
    }
    while (height > 0) {
        /* Independently allocated SDL-owned surfaces cannot alias. External
         * PREALLOC storage can, so retain memmove for that case rather than
         * making assumptions about the caller's surface buffers. */
        if (disjoint) SDL_memcpy(dst, src, (size_t)row_octets);
        else SDL_memmove(dst, src, (size_t)row_octets);
        if (--height > 0) {
            src += src_pitch;
            dst += dst_pitch;
        }
    }
}

static void indexed_pixels(SDL_BlitInfo *info, int key)
{
    int bpp = info->dst->BytesPerPixel;
    int map_stride = bpp == 3 ? 4 : bpp;
    Uint8 *srcrow = info->s_pixels;
    Uint8 *dstrow = info->d_pixels;
    long spitch = (long)info->s_width + info->s_skip;
    long dpitch = (long)info->d_width * bpp + info->d_skip;
    int y;
    for (y = 0; y < info->d_height; ++y) {
        Uint8 *src = srcrow;
        Uint8 *dst = dstrow;
        int x;
        for (x = 0; x < info->d_width; ++x) {
            unsigned index = *src++ & 255;
            if (!key || index != info->src->colorkey) {
                const Uint8 *entry = info->table + index * map_stride;
                /* These are octet carriers, not a Uint16/32 reinterpretation.
                 * Palette mapping is precomputed once per surface mapping. */
                switch (bpp) {
                    case 4: dst[3] = entry[3]; /* fall through */
                    case 3: dst[2] = entry[2]; /* fall through */
                    case 2: dst[1] = entry[1]; /* fall through */
                    case 1: dst[0] = entry[0]; break;
                }
            }
            dst += bpp;
        }
        srcrow += spitch;
        dstrow += dpitch;
    }
}

/* A frequent SDL shadow-surface conversion is XRGB8888 -> BGR24. The
 * channels already occupy the same three octets, so loading a wide pixel,
 * unpacking three channels and packing them again is unnecessary. Keep
 * octet-carrier accesses rather than treating the storage as native words.
 * The reverse conversion also handles RGB24 -> ARGB8888; it writes exactly
 * the same alpha/unused bits as the general opaque path. */
static void rgb24_pixels(SDL_BlitInfo *info)
{
    int sbpp = info->src->BytesPerPixel;
    int dbpp = info->dst->BytesPerPixel;
    Uint8 fourth = info->dst->Amask ? (Uint8)(info->src->alpha & 255) : 0;
    Uint8 *srcrow = info->s_pixels;
    Uint8 *dstrow = info->d_pixels;
    long spitch = (long)info->s_width * sbpp + info->s_skip;
    long dpitch = (long)info->d_width * dbpp + info->d_skip;
    int y;
    for (y = 0; y < info->d_height; ++y) {
        const Uint8 *src = srcrow;
        Uint8 *dst = dstrow;
        int x;
        if (sbpp == 4) {
            for (x = 0; x < info->d_width; ++x) {
#if SDL_BYTEORDER == SDL_LIL_ENDIAN
                dst[0] = src[0] & 255;
                dst[1] = src[1] & 255;
                dst[2] = src[2] & 255;
#else
                dst[0] = src[1] & 255;
                dst[1] = src[2] & 255;
                dst[2] = src[3] & 255;
#endif
                src += 4;
                dst += 3;
            }
        } else {
            for (x = 0; x < info->d_width; ++x) {
#if SDL_BYTEORDER == SDL_LIL_ENDIAN
                dst[0] = src[0] & 255;
                dst[1] = src[1] & 255;
                dst[2] = src[2] & 255;
                dst[3] = fourth;
#else
                dst[0] = fourth;
                dst[1] = src[0] & 255;
                dst[2] = src[1] & 255;
                dst[3] = src[2] & 255;
#endif
                src += 3;
                dst += 4;
            }
        }
        srcrow += spitch;
        dstrow += dpitch;
    }
}

/* Matching RGB24 surfaces can blend their octets directly. These are the
 * generic SDL /256 +255 rounding rules, including keyed surface alpha, not
 * the distinct 32-bit identity-alpha rounding path. Unsigned intermediates
 * need only sixteen bits: s*a + d*(256-a) +255 is at most 65535. */
static void rgb24_key_alpha_pixels(SDL_BlitInfo *info, int alpha, int key)
{
    unsigned a = info->src->alpha & 255U;
    unsigned inverse = 256U - a;
    Uint8 key_octets[3];
    Uint8 *srcrow = info->s_pixels;
    Uint8 *dstrow = info->d_pixels;
    long spitch = (long)info->s_width * 3 + info->s_skip;
    long dpitch = (long)info->d_width * 3 + info->d_skip;
    int y;
    if (alpha && !a) return;
    /* A 24-bit pixel cannot equal a key with any high bits set. Retain that
     * full-pixel comparison behavior instead of silently truncating the key. */
    if (key && (info->src->colorkey & 0xff000000UL)) key = 0;
    SDL_C16StorePixel(key_octets, 3, info->src->colorkey);
    for (y = 0; y < info->d_height; ++y) {
        const Uint8 *src = srcrow;
        Uint8 *dst = dstrow;
        int x;
        for (x = 0; x < info->d_width; ++x) {
            unsigned s0 = src[0] & 255U;
            unsigned s1 = src[1] & 255U;
            unsigned s2 = src[2] & 255U;
            if (!key || s0 != key_octets[0]
                || s1 != key_octets[1] || s2 != key_octets[2]) {
                if (alpha) {
                    unsigned d0 = dst[0] & 255U;
                    unsigned d1 = dst[1] & 255U;
                    unsigned d2 = dst[2] & 255U;
                    dst[0] = (Uint8)((s0 * a + d0 * inverse + 255U) >> 8);
                    dst[1] = (Uint8)((s1 * a + d1 * inverse + 255U) >> 8);
                    dst[2] = (Uint8)((s2 * a + d2 * inverse + 255U) >> 8);
                } else {
                    dst[0] = (Uint8)s0;
                    dst[1] = (Uint8)s1;
                    dst[2] = (Uint8)s2;
                }
            }
            /* Cache all source/destination octets before stores and keep the
             * generic forward traversal even when external buffers alias. */
            src += 3;
            dst += 3;
        }
        srcrow += spitch;
        dstrow += dpitch;
    }
}

static void blit_pixels(SDL_BlitInfo *info)
{
    SDL_Surface *source = (SDL_Surface *)info->aux_data;
    const SDL_PixelFormat *sf = info->src;
    const SDL_PixelFormat *df = info->dst;
    int sbpp = sf->BytesPerPixel;
    int dbpp = df->BytesPerPixel;
    int alpha = (source->flags & SDL_SRCALPHA)
        && (sf->Amask || sf->alpha != SDL_ALPHA_OPAQUE);
    int pixel_alpha = alpha && sf->Amask;
    int key = (source->flags & SDL_SRCCOLORKEY) && !pixel_alpha;
    int identity = source->map->identity;
    int same16 = alpha && !key && !pixel_alpha && identity && rgb16(sf);
    int same888 = alpha && !key && sbpp == 4 && dbpp == 4
        && sf->Rloss == 0 && sf->Gloss == 0 && sf->Bloss == 0
        && (sf->Rmask | sf->Gmask | sf->Bmask) == 0x00ffffffUL
        && sf->Rmask == df->Rmask && sf->Gmask == df->Gmask && sf->Bmask == df->Bmask
        && (!pixel_alpha || sf->Amask == 0xff000000UL)
        && (!df->Amask || df->Amask == 0xff000000UL);
    int argb_to16 = pixel_alpha && sf->Amask == 0xff000000UL
        && rgb888(sf) && rgb16(df);
    int expand565 = !alpha && !key && sf->BytesPerPixel == 2
        && sf->Rmask == 0xf800UL && sf->Gmask == 0x07e0UL
        && sf->Bmask == 0x001fUL && dbpp == 4 && df->Amask
        && df->Rloss == 0 && df->Gloss == 0 && df->Bloss == 0
        && (df->Rshift & 7) == 0 && (df->Gshift & 7) == 0
        && (df->Bshift & 7) == 0;
    Uint8 *srcrow = info->s_pixels;
    Uint8 *dstrow = info->d_pixels;
    long spitch = (long)info->s_width * sbpp + info->s_skip;
    long dpitch = (long)info->d_width * dbpp + info->d_skip;
    int y;

    if (identity && !alpha && !key) {
        copy_pixels(info, source);
        return;
    }
    if (sf->palette && !alpha && info->table) {
        indexed_pixels(info, key);
        return;
    }
    if (sbpp == 3 && dbpp == 3 && !sf->palette && !df->palette
        && !sf->Amask && !df->Amask
        && sf->alpha <= 255U
        && sf->Rloss == 0 && sf->Gloss == 0 && sf->Bloss == 0
        && df->Rloss == 0 && df->Gloss == 0 && df->Bloss == 0
        && sf->Rmask == df->Rmask && sf->Gmask == df->Gmask
        && sf->Bmask == df->Bmask
        && (sf->Rmask | sf->Gmask | sf->Bmask) == 0x00ffffffUL) {
        rgb24_key_alpha_pixels(info, alpha, key);
        return;
    }
    if (!alpha && !key && !sf->palette && !df->palette
        && ((sbpp == 4 && dbpp == 3 && !df->Amask)
         || (sbpp == 3 && dbpp == 4 && !sf->Amask
             && (!df->Amask || df->Amask == 0xff000000UL)))
        && sf->Rloss == 0 && sf->Gloss == 0 && sf->Bloss == 0
        && df->Rloss == 0 && df->Gloss == 0 && df->Bloss == 0
        && sf->Rmask == df->Rmask && sf->Gmask == df->Gmask
        && sf->Bmask == df->Bmask
        && (sf->Rmask | sf->Gmask | sf->Bmask) == 0x00ffffffUL) {
        rgb24_pixels(info);
        return;
    }
    for (y = 0; y < info->d_height; ++y) {
        const Uint8 *src = srcrow;
        Uint8 *dst = dstrow;
        int x;
        for (x = 0; x < info->d_width; ++x) {
            Uint32 sp = SDL_C16LoadPixel(src, sbpp);
            Uint32 dp, output;
            unsigned sr, sg, sb, dr, dg, db, a, da;
            if (key && sp == sf->colorkey) goto next_pixel;
            if (identity && !alpha && sbpp == 2) {
                SDL_C16StorePixel(dst, dbpp, sp);
                goto next_pixel;
            }
            if (same16) {
                dp = SDL_C16LoadPixel(dst, dbpp);
                output = blend16(sp, dp, sf, sf->alpha, 1);
                SDL_C16StorePixel(dst, dbpp, output);
                goto next_pixel;
            }
            if (argb_to16) {
                /* SDL's ARGB8888->565/555 path blends five-bit alpha in
                 * the packed destination channels, including its opaque
                 * threshold at 248 rather than 255. */
                a = (unsigned)(sp >> 27);
                if (a) {
                    Uint32 packed;
                    dp = SDL_C16LoadPixel(dst, dbpp);
                    if (df->Gmask == 0x07e0UL) {
                        packed = ((sp >> 8) & 0xf800UL)
                               | ((sp >> 5) & 0x07e0UL)
                               | ((sp >> 3) & 0x001fUL);
                    } else {
                        packed = ((sp >> 9) & 0x7c00UL)
                               | ((sp >> 6) & 0x03e0UL)
                               | ((sp >> 3) & 0x001fUL);
                    }
                    output = a == 31 ? packed : blend16(packed, dp, df, a << 3, 0);
                    SDL_C16StorePixel(dst, dbpp, output);
                }
                goto next_pixel;
            }
            /* Palette mappings are scalar octets; the true-color map table
             * is intentionally not reinterpreted as packed C integers. */
            if (sf->palette && df->palette && !alpha) {
                *dst = info->table ? info->table[sp & 255] : (Uint8)sp;
                goto next_pixel;
            }
            unpack(sp, sf, &sr, &sg, &sb);
            da = sf->Amask
                ? channel(sp, sf->Amask, sf->Ashift, sf->Aloss) : sf->alpha;
            if (alpha) {
                a = pixel_alpha ? da : sf->alpha;
                if (!a && !sf->palette && !df->palette
                    && (!same888 || pixel_alpha)) goto next_pixel;
                dp = SDL_C16LoadPixel(dst, dbpp);
                unpack(dp, df, &dr, &dg, &db);
                if (same888) {
                    /* Match upstream's common 8888 fast paths: floor /256,
                     * opaque pixel-alpha copies, destination alpha retained. */
                    if (pixel_alpha && a == 255) {
                        dr = sr; dg = sg; db = sb;
                    } else {
                        dr = (unsigned)(((Sint32)sr * a + (Sint32)dr * (256-a)) / 256);
                        dg = (unsigned)(((Sint32)sg * a + (Sint32)dg * (256-a)) / 256);
                        db = (unsigned)(((Sint32)sb * a + (Sint32)db * (256-a)) / 256);
                    }
                    output = pack(df, dr, dg, db, 0)
                           | (pixel_alpha ? dp & 0xff000000UL : 0xff000000UL);
                    SDL_C16StorePixel(dst, dbpp, output);
                    goto next_pixel;
                } else {
                    dr = blend(sr, dr, a);
                    dg = blend(sg, dg, a);
                    db = blend(sb, db, a);
                }
                sr = dr; sg = dg; sb = db;
                if (pixel_alpha) {
                    da = df->Amask
                        ? channel(dp, df->Amask, df->Ashift, df->Aloss) : 0;
                } else {
                    /* SDL's indexed-alpha blitter writes RGB only. */
                    da = sf->palette ? 0 : 255;
                }
            } else if (expand565) {
                /* Reproduce SDL's 565 lookup tables, which add independently
                 * scaled low/high octets (including their rounding). */
                unsigned green = (unsigned)((sp >> 5) & 63);
                sr = (unsigned)(((sp >> 11) & 31) * 255 / 31);
                sg = ((green & 7) * 255 / 63) + ((green >> 3) * 2040 / 63);
                sb = (unsigned)((sp & 31) * 255 / 31);
                output = pack(df, sr, sg, sb, 255);
                output |= 0xffffffffUL & ~(df->Rmask | df->Gmask | df->Bmask);
                SDL_C16StorePixel(dst, dbpp, output);
                goto next_pixel;
            }
            if (df->palette) {
                unsigned index = ((sr >> 5) << 5) | ((sg >> 5) << 2) | (sb >> 6);
                output = info->table ? info->table[index] : index;
            } else {
                output = pack(df, sr, sg, sb, da);
            }
            SDL_C16StorePixel(dst, dbpp, output);
next_pixel:
            src += sbpp;
            dst += dbpp;
        }
        srcrow += spitch;
        dstrow += dpitch;
    }
}

SDL_loblit SDL_C16CalculateBlit(SDL_Surface *surface)
{
    SDL_PixelFormat *sf = surface->format;
    SDL_PixelFormat *df = surface->map->dst->format;
    if (sf->BitsPerPixel < 8 || df->BitsPerPixel < 8
        || sf->BytesPerPixel < 1 || sf->BytesPerPixel > 4
        || df->BytesPerPixel < 1 || df->BytesPerPixel > 4) return NULL;
    /* Upstream SDL 1.2 likewise rejects indexed->indexed alpha blending. */
    if (sf->palette && df->palette && (surface->flags & SDL_SRCALPHA)
        && sf->alpha != SDL_ALPHA_OPAQUE) return NULL;
    surface->map->sw_data->aux_data = surface;
    return blit_pixels;
}

int SDL_C16FillRect(SDL_Surface *dst, SDL_Rect *dstrect, Uint32 color)
{
    SDL_Rect clipped;
    long left, top, right, bottom, clip_right, clip_bottom;
    Uint8 *row, *first;
    unsigned long row_octets, filled;
    int bpp, y;
    if (!dst || !dst->format) {
        SDL_SetError("Invalid fill surface");
        return -1;
    }
    bpp = dst->format->BytesPerPixel;
    if (dst->format->BitsPerPixel < 8 || bpp < 1 || bpp > 4) {
        SDL_SetError("Fill rect on unsupported surface format");
        return -1;
    }
    clipped = dstrect ? *dstrect : dst->clip_rect;
    left = clipped.x; top = clipped.y;
    right = left + clipped.w; bottom = top + clipped.h;
    clip_right = (long)dst->clip_rect.x + dst->clip_rect.w;
    clip_bottom = (long)dst->clip_rect.y + dst->clip_rect.h;
    if (left < dst->clip_rect.x) left = dst->clip_rect.x;
    if (top < dst->clip_rect.y) top = dst->clip_rect.y;
    if (right > clip_right) right = clip_right;
    if (bottom > clip_bottom) bottom = clip_bottom;
    clipped.x = (Sint16)left; clipped.y = (Sint16)top;
    clipped.w = (Uint16)(right > left ? right - left : 0);
    clipped.h = (Uint16)(bottom > top ? bottom - top : 0);
    if (dstrect) *dstrect = clipped;
    if (!clipped.w || !clipped.h) return 0;
    if (SDL_LockSurface(dst) < 0) return -1;
    row = (Uint8 *)dst->pixels + top * dst->pitch + left * bpp;
    first = row;
    row_octets = (unsigned long)clipped.w * bpp;
    SDL_C16StorePixel(first, bpp, color);
    filled = bpp;
    /* Seed one pixel, then double the initialized prefix with nonoverlapping
     * copies. This uses the libc's bank-aware bulk path on C16 and ordinary
     * memcpy on byte-addressed builds. Copies are bounded to one row, keeping
     * pitch padding and cells outside the clipped rectangle untouched. */
    while (filled < row_octets) {
        unsigned long count = row_octets - filled;
        if (count > filled) count = filled;
        SDL_memcpy(first + filled, first, (size_t)count);
        filled += count;
    }
    for (y = 1; y < clipped.h; ++y) {
        row += dst->pitch;
        SDL_memcpy(row, first, (size_t)row_octets);
    }
    SDL_UnlockSurface(dst);
    return 0;
}
