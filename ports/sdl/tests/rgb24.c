/* Exact RGB24 key/surface-alpha regression on upstream, host adapter and C16.
 * Entire storage is compared, including padding and clipped-out pixels.
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "SDL_blit.h"
#include "pixels.h"

extern int SDL_MapSurface(SDL_Surface *, SDL_Surface *);
static int force_adapter, cases;
static Uint32 aggregate = 2166136261UL;

static void require(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "SDL_RGB24 case=%d failed: %s: %s\n", cases, message, SDL_GetError());
        exit(1);
    }
}

static Uint32 hash(const Uint8 *data, unsigned long size)
{
    Uint32 value = 2166136261UL;
    unsigned long i;
    for (i = 0; i < size; ++i) value = (value ^ data[i]) * 16777619UL;
    return value;
}

static void store(Uint8 *data, Uint32 pixel)
{
#if SDL_BYTEORDER == SDL_LIL_ENDIAN
    data[0] = pixel & 255; data[1] = (pixel >> 8) & 255; data[2] = (pixel >> 16) & 255;
#else
    data[2] = pixel & 255; data[1] = (pixel >> 8) & 255; data[0] = (pixel >> 16) & 255;
#endif
}

static Uint32 load(const Uint8 *data)
{
#if SDL_BYTEORDER == SDL_LIL_ENDIAN
    return (Uint32)data[0] | ((Uint32)data[1] << 8) | ((Uint32)data[2] << 16);
#else
    return (Uint32)data[2] | ((Uint32)data[1] << 8) | ((Uint32)data[0] << 16);
#endif
}

static void compare_forward(const Uint8 *source, Uint8 *destination,
                             unsigned pitch, int width, int alpha, int key_mode)
{
    int x, y;
    Uint32 key = key_mode == 3 ? 0x01000000UL : key_mode == 2 ? 0x562341UL : 0;
    for (y = 0; y < 2; ++y) for (x = 0; x < width - 6; ++x) {
        const Uint8 *src = source + (unsigned long)y * pitch + (unsigned long)(x + 2) * 3;
        Uint8 *dst = destination + (unsigned long)(y + 1) * pitch + (unsigned long)(x + 3) * 3;
        Uint32 pixel = load(src);
        if (key_mode && pixel == key) continue;
        if (alpha < 0 || alpha == 255) store(dst, pixel);
        else if (alpha) {
            Uint8 out[3];
            int i;
            /* A wide signed reference independently checks the narrow fast
             * path, with source and destination cached before any write. */
            for (i = 0; i < 3; ++i)
                out[i] = (Uint8)(((Sint32)src[i] * alpha
                    + (Sint32)dst[i] * (256 - alpha) + 255) / 256);
            memcpy(dst, out, 3);
        }
    }
}

static void run(int order, int alpha, int key_mode, int width, int alias_delta)
{
    static const int shifts[6][3] = {{16,8,0},{0,8,16},{16,0,8},
                                    {8,16,0},{8,0,16},{0,16,8}};
    unsigned pitch = (unsigned)width * 3 + 7;
    unsigned long size;
    Uint8 *source_memory = NULL, *dest_memory = NULL, *expected, *source, *dest;
    SDL_Surface *s, *d;
    SDL_Rect sr = {1,0,0,2}, dr = {2,1,0,0}, clip = {3,0,0,3};
    Uint32 masks[3], source_hash;
    int x, y;
    unsigned long i;
    sr.w = width - 3;
    clip.w = width - 6;
    for (x = 0; x < 3; ++x) masks[x] = 255UL << shifts[order][x];
    if (pitch > 32767U) {
        require(alias_delta == 0, "wide alias fixture unsupported");
        s = SDL_CreateRGBSurface(0, width, 3, 24, masks[0], masks[1], masks[2], 0);
        d = SDL_CreateRGBSurface(0, width, 3, 24, masks[0], masks[1], masks[2], 0);
        require(s && d, "create wide owned surfaces");
        require(s->pitch == d->pitch, "equal wide pitches");
        pitch = s->pitch;
        source = s->pixels; dest = d->pixels;
    } else {
        size = (unsigned long)pitch * 3;
        source_memory = malloc(size + 64);
        dest_memory = alias_delta ? source_memory : malloc(size + 64);
        require(source_memory && dest_memory, "allocate guarded buffers");
        memset(source_memory, 0xb6, size + 64);
        memset(dest_memory, 0xb6, size + 64);
        source = source_memory + 16;
        dest = dest_memory + 16 + alias_delta;
        s = SDL_CreateRGBSurfaceFrom(source, width, 3, 24, (int)pitch, masks[0], masks[1], masks[2], 0);
        d = SDL_CreateRGBSurfaceFrom(dest, width, 3, 24, (int)pitch, masks[0], masks[1], masks[2], 0);
        require(s && d, "create padded external surfaces");
    }
    size = (unsigned long)pitch * 3;
    for (i = 0; i < size; ++i) dest[i] = (Uint8)((i * 37UL + i / 89 + 117) & 255);
    for (i = 0; i < size; ++i) source[i] = (Uint8)((i * 73UL + i / 41 + 3) & 255);
    for (y = 0; y < 3; ++y) for (x = 0; x < width; x += 7)
        store(source + (unsigned long)y * pitch + (unsigned long)x * 3,
              x % 2 ? 0x562341UL : 0);
    source_hash = hash(source, size);
    expected = malloc(size + 64);
    require(expected != NULL, "allocate expected storage");
    if (alias_delta) {
        memcpy(expected, source_memory, size + 64);
        compare_forward(expected + 16, expected + 16 + alias_delta,
                          pitch, width, alpha, key_mode);
    } else {
        memcpy(expected, dest, size);
        compare_forward(source, expected, pitch, width, alpha, key_mode);
    }
    require(SDL_SetAlpha(s, alpha < 0 ? 0 : SDL_SRCALPHA, alpha < 0 ? 255 : alpha) == 0,
            "set source alpha");
    require(SDL_SetColorKey(s, key_mode ? SDL_SRCCOLORKEY : 0,
            key_mode == 3 ? 0x01000000UL : key_mode == 2 ? 0x562341UL : 0) == 0, "set key");
    require(SDL_SetClipRect(d, &clip), "set destination clip");
    if (force_adapter) {
        require(SDL_MapSurface(s, d) == 0, "map host adapter");
        s->map->sw_data->blit = SDL_C16CalculateBlit(s);
        require(s->map->sw_data->blit != NULL, "select host adapter");
    }
    require(SDL_BlitSurface(s, &sr, d, &dr) == 0, "RGB24 blit");
    require(dr.x == 3 && dr.y == 1 && dr.w == width - 6 && dr.h == 2,
            "updated clipped rectangle");
    if (alias_delta) {
        require(memcmp(source_memory, expected, size + 64) == 0, "forward alias storage and guards");
    } else {
        require(memcmp(dest, expected, size) == 0, "pixels, padding and clipping guards");
        require(hash(source, size) == source_hash, "source unchanged");
        if (dest_memory) for (i = 0; i < 16; ++i) {
            require(dest_memory[i] == 0xb6, "leading guard");
            require(dest_memory[16 + size + i] == 0xb6, "trailing guard");
        }
    }
    aggregate = (aggregate ^ hash(dest, size)) * 16777619UL;
    ++cases;
    SDL_FreeSurface(s); SDL_FreeSurface(d);
    free(expected);
    if (dest_memory != source_memory) free(dest_memory);
    free(source_memory);
}

int main(int argc, char **argv)
{
    static const int alphas[] = {-1,0,1,64,128,254,255};
    static const int deltas[] = {-3,-1,1,3};
    int order, a, key, delta;
    force_adapter = argc > 1 && !strcmp(argv[1], "--adapter");
    for (order = 0; order < 6; ++order) for (a = 0; a < 7; ++a)
        for (key = 0; key < 4; ++key) run(order, alphas[a], key, 17, 0);
    for (order = 0; order < 6; ++order) for (a = 0; a < 2; ++a)
        for (delta = 0; delta < 4; ++delta) run(order, a ? 255 : 64, 1, 17, deltas[delta]);
    for (order = 0; order < 3; ++order) for (a = 0; a < 3; ++a)
        for (key = 1; key <= 3; key += 2) run(order, a == 0 ? 0 : a == 1 ? 64 : 255, key, 12001, 0);
    printf("SDL_RGB24 cases=%d hash=%08lx passed\n", cases, (unsigned long)aggregate);
    return 0;
}
