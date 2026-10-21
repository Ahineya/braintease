/* Public SDL surface regressions for bulk fills/copies and display conversion.
 * Odd pitches, subrectangles and rows over 32 KiB expose errors hidden by the
 * small pixel matrix. The same application runs against upstream and C16 SDL.
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GUARD 13
#define SENTINEL 0xd7

static int cases;

static void require(int condition, const char *name)
{
    if (!condition) {
        fprintf(stderr, "SDL_BULK failed: %s: %s\n", name, SDL_GetError());
        exit(1);
    }
}

static Uint8 octet(Uint32 pixel, int position, int bpp)
{
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
    position = bpp - position - 1;
#else
    (void)bpp;
#endif
    return (Uint8)((pixel >> (position * 8)) & 255);
}

static SDL_Surface *external(Uint8 *pixels, int width, int depth,
                             unsigned pitch, int alpha)
{
    SDL_Surface *s;
    Uint32 r = depth == 8 ? 0 : depth == 16 ? 0xf800UL : 0xff0000UL;
    Uint32 g = depth == 8 ? 0 : depth == 16 ? 0x07e0UL : 0x00ff00UL;
    Uint32 b = depth == 8 ? 0 : depth == 16 ? 0x001fUL : 0x0000ffUL;
    s = SDL_CreateRGBSurfaceFrom(pixels, width, 5, depth, (int)pitch,
                                 r, g, b, alpha ? 0xff000000UL : 0);
    require(s != NULL, "create external surface");
    return s;
}

static void guards(const Uint8 *data, unsigned long size)
{
    int i;
    for (i = 0; i < GUARD; ++i) {
        require(data[i] == SENTINEL, "leading guard");
        require(data[GUARD + size + i] == SENTINEL, "trailing guard");
    }
}

struct fixture {
    SDL_Surface *surface;
    Uint8 *allocation;
    Uint8 *pixels;
    unsigned long size;
};

static struct fixture fixture(int width, int depth, int padding, int alpha)
{
    struct fixture f;
    unsigned pitch = (unsigned)width * (depth / 8) + padding;
    f.allocation = NULL;
    if (pitch > 32767U) {
        /* SDL's external-buffer API accepts signed int pitch. Its C16
         * signature cannot express these rows, whereas SDL-owned surfaces
         * calculate/store their pitch directly in a Uint16. */
        f.surface = SDL_CreateRGBSurface(0, width, 5, depth,
                         0xff0000UL, 0xff00UL, 0xffUL,
                         alpha ? 0xff000000UL : 0);
        require(f.surface != NULL, "create wide owned surface");
        f.pixels = f.surface->pixels;
        f.size = (unsigned long)f.surface->pitch * f.surface->h;
    } else {
        f.size = (unsigned long)pitch * 5;
        f.allocation = malloc(f.size + 2 * GUARD);
        require(f.allocation != NULL, "allocate guarded surface");
        memset(f.allocation, SENTINEL, f.size + 2 * GUARD);
        f.pixels = f.allocation + GUARD;
        f.surface = external(f.pixels, width, depth, pitch, alpha);
    }
    memset(f.pixels, SENTINEL, f.size);
    return f;
}

static void release(struct fixture *f)
{
    if (f->allocation) guards(f->allocation, f->size);
    SDL_FreeSurface(f->surface);
    free(f->allocation);
}

static void fills(int depth, int width)
{
    int bpp = depth / 8, y;
    struct fixture f = fixture(width, depth, 7, 0);
    SDL_Surface *s = f.surface;
    unsigned pitch = s->pitch;
    SDL_Rect clip, rect = {-2, 0, 0, 5};
    Uint32 color = 0x9e4d7ac3UL;
    clip.x = width > 3 ? 1 : 0;
    clip.y = 1;
    clip.w = width > 3 ? width - 2 : width;
    clip.h = 3;
    rect.w = width + 4;
    require(SDL_SetClipRect(s, &clip), "set fill clip");
    require(SDL_FillRect(s, &rect, color) == 0, "bulk fill");
    require(rect.x == clip.x && rect.y == clip.y
         && rect.w == clip.w && rect.h == clip.h, "clipped fill rectangle");
    for (y = 0; y < 5; ++y) {
        unsigned offset;
        for (offset = 0; offset < pitch; ++offset) {
            unsigned x = offset / bpp;
            Uint8 expected = SENTINEL;
            if (y >= clip.y && y < clip.y + clip.h
                && x >= (unsigned)clip.x && x < (unsigned)clip.x + clip.w)
                expected = octet(color, offset % bpp, bpp);
            require(f.pixels[(unsigned long)y * pitch + offset] == expected,
                    "fill pixels, padding and clip guards");
        }
    }
    release(&f);
    ++cases;
}

static Uint32 color_at(int x, int y)
{
    return (((Uint32)(x + y * 13) & 255) << 16)
         | (((Uint32)(x * 3UL + y * 7) & 255) << 8)
         | ((Uint32)(x * 5UL + y * 11) & 255);
}

static void conversions(int source_bpp, int destination_bpp, int alpha, int width)
{
    int y;
    struct fixture source = fixture(width, source_bpp * 8, 7, source_bpp == 4);
    struct fixture dest = fixture(width, destination_bpp * 8, 9, alpha);
    SDL_Surface *s = source.surface, *d = dest.surface;
    unsigned spitch = s->pitch, dpitch = d->pitch;
    SDL_Rect sr = {3, 1, 0, 3}, dr = {5, 1, 0, 0};
    sr.w = width - 11;
    require(SDL_SetAlpha(s, 0, 255) == 0, "disable source blending");
    for (y = 0; y < 5; ++y) {
        int x, i;
        for (x = 0; x < width; ++x) for (i = 0; i < source_bpp; ++i)
            source.pixels[(unsigned long)y * spitch + (unsigned long)x * source_bpp + i]
                = octet(color_at(x, y) | 0x6b000000UL, i, source_bpp);
    }
    require(SDL_BlitSurface(s, &sr, d, &dr) == 0, "wide RGB conversion");
    require(dr.w == sr.w && dr.h == sr.h, "conversion destination rectangle");
    for (y = 0; y < 5; ++y) {
        unsigned offset;
        for (offset = 0; offset < dpitch; ++offset) {
            unsigned x = offset / destination_bpp;
            int i = offset % destination_bpp;
            Uint8 expected = SENTINEL;
            if (y >= dr.y && y < dr.y + dr.h
                && x >= (unsigned)dr.x && x < (unsigned)dr.x + dr.w) {
                Uint32 color = color_at(x - dr.x + sr.x, y - dr.y + sr.y);
                /* Upstream's unused high octet can differ from C16's defined
                 * zero. Alpha channels themselves must still match exactly. */
                if (destination_bpp == 4 && !alpha
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
                    && i == 0
#else
                    && i == 3
#endif
                    ) continue;
                if (alpha) color |= 0xff000000UL;
                expected = octet(color, i, destination_bpp);
            }
            require(dest.pixels[(unsigned long)y * dpitch + offset] == expected,
                    "converted pixels, padding and rectangle guards");
        }
    }
    release(&source);
    release(&dest);
    ++cases;
}

static void copies(int direction)
{
    SDL_Surface *s = SDL_CreateRGBSurface(0, 12001, 5, 32,
                                         0xff0000UL, 0xff00UL, 0xffUL, 0);
    SDL_Surface *d;
    SDL_Rect sr = {1, 0, 11998, 4}, dr = {1, 0, 0, 0};
    unsigned long size, i;
    Uint8 *before, *expected;
    require(s != NULL, "create owned copy surface");
    size = (unsigned long)s->pitch * s->h;
    before = malloc(size);
    expected = malloc(size);
    require(before != NULL && expected != NULL, "allocate copy fixtures");
    for (i = 0; i < size; ++i) before[i] = (Uint8)((i * 37UL + i / 89) & 255);
    memcpy(s->pixels, before, size);
    memcpy(expected, before, size);
    d = s;
    if (direction == 0) dr.x = 2;
    if (direction == 1) { sr.x = 2; dr.x = 1; }
    if (direction == 2) dr.y = 1;
    if (direction == 3) { sr.y = 1; dr.y = 0; }
    if (direction == 4) {
        d = SDL_CreateRGBSurface(0, 12001, 5, 32,
                                  0xff0000UL, 0xff00UL, 0xffUL, 0);
        require(d != NULL, "create distinct owned destination");
        memcpy(d->pixels, before, size);
    }
    if (direction == 5) {
        /* Distinct SDL objects can refer to the same external allocation. */
        /* Alias a narrow row without narrowing the signed external-pitch API. */
        d = SDL_CreateRGBSurfaceFrom(s->pixels, 8001, 1, 32, 32764,
                                     0xff0000UL, 0xff00UL, 0xffUL, 0);
        require(d != NULL, "create external alias destination");
        sr.w = 7998;
        sr.h = 1;
        dr.x = 2;
    }
    {
        int y;
        for (y = 0; y < sr.h; ++y) {
            unsigned long source = (unsigned long)(sr.y + y) * s->pitch + (unsigned long)sr.x * 4;
            unsigned long dest = (unsigned long)(dr.y + y) * d->pitch + (unsigned long)dr.x * 4;
            unsigned long octets = (unsigned long)sr.w * 4;
            for (i = 0; i < octets; ++i) expected[dest + i] = before[source + i];
        }
    }
    require(SDL_BlitSurface(s, &sr, d, &dr) == 0, "owned or overlapping copy");
    if (memcmp(d->pixels, expected, size) != 0) {
        for (i = 0; i < size; ++i) if (((Uint8 *)d->pixels)[i] != expected[i]) break;
        fprintf(stderr, "SDL_BULK copy direction=%d first_difference=%lu\n", direction, i);
    }
    require(memcmp(d->pixels, expected, size) == 0, "copy storage and outside guards");
    if (direction == 4) require(memcmp(s->pixels, before, size) == 0, "copy source preserved");
    if (d != s) SDL_FreeSurface(d);
    SDL_FreeSurface(s);
    free(before);
    free(expected);
    ++cases;
}

int main(void)
{
    static const int depths[] = {8, 16, 24, 32};
    static const int widths[] = {1, 2, 3, 7, 129, 12001};
    int d, w;
    for (d = 0; d < 4; ++d) for (w = 0; w < 6; ++w) fills(depths[d], widths[w]);
    for (w = 0; w < 2; ++w) {
        int width = w ? 12001 : 129;
        conversions(4, 3, 0, width);
        conversions(3, 4, 0, width);
        conversions(3, 4, 1, width);
    }
    for (d = 0; d < 6; ++d) copies(d);
    printf("SDL_BULK cases=%d passed\n", cases);
    return 0;
}
