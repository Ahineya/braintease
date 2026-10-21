/* Differential host test: ordinary upstream SDL blitters versus the C16
 * octet-carrier adapter. Link both into the same reference executable. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "SDL_video.h"
#include "SDL_blit.h"
#include "pixels.h"

extern int SDL_MapSurface(SDL_Surface *, SDL_Surface *);

static unsigned random_state = 512;
static unsigned random_value(void)
{
    random_state = random_state * 1664525U + 1013904223U;
    return random_state;
}

static SDL_Surface *make_surface(int format)
{
    SDL_Surface *surface;
    switch (format) {
        case 0: surface = SDL_CreateRGBSurface(0, 7, 3, 8, 0, 0, 0, 0); break;
        case 1: surface = SDL_CreateRGBSurface(0, 7, 3, 16, 0xf800, 0x07e0, 0x001f, 0); break;
        case 2: surface = SDL_CreateRGBSurface(0, 7, 3, 16, 0x7c00, 0x03e0, 0x001f, 0); break;
        case 3: surface = SDL_CreateRGBSurface(0, 7, 3, 24, 0xff0000, 0xff00, 0xff, 0); break;
        case 4: surface = SDL_CreateRGBSurface(0, 7, 3, 24, 0xff, 0xff00, 0xff0000, 0); break;
        case 5: surface = SDL_CreateRGBSurface(0, 7, 3, 32, 0xff0000, 0xff00, 0xff, 0); break;
        case 6: surface = SDL_CreateRGBSurface(0, 7, 3, 32, 0xff0000, 0xff00, 0xff, 0xff000000); break;
        default: surface = SDL_CreateRGBSurface(0, 7, 3, 32, 0xff, 0xff00, 0xff0000, 0xff000000); break;
    }
    if (surface && surface->format->palette) {
        int i;
        for (i = 0; i < 256; ++i) {
            SDL_Color color;
            color.r = (Uint8)i;
            color.g = (Uint8)(255-i);
            color.b = (Uint8)(i^127);
            color.unused = 0;
            SDL_SetColors(surface, &color, i, 1);
        }
    }
    return surface;
}

int main(void)
{
    static const unsigned alphas[] = {0, 1, 64, 128, 200, 254, 255};
    int si, di, am, key, cases = 0, failures = 0;
    int unused_cases = 0, alpha_correction_cases = 0;
    for (si = 0; si < 8; ++si) for (di = 0; di < 8; ++di)
    for (am = -1; am < 7; ++am) for (key = 0; key < 2; ++key) {
        SDL_Surface *source = make_surface(si);
        SDL_Surface *reference = make_surface(di);
        SDL_Surface *actual = make_surface(di);
        SDL_Rect sr = {0, 0, 7, 3}, rr = {0, 0, 7, 3}, ar = {0, 0, 7, 3};
        Uint8 initial[128];
        size_t i, src_size, dst_size;
        int x, y, bad = 0, alpha_correction = 0, unused = 0;
        int pixel_alpha;
        if (!source || !reference || !actual) return 2;
        src_size = source->pitch * source->h;
        dst_size = actual->pitch * actual->h;
        if (dst_size > sizeof(initial)) return 2;
        for (i = 0; i < src_size; ++i) ((Uint8 *)source->pixels)[i] = random_value() >> 24;
        for (i = 0; i < dst_size; ++i) {
            initial[i] = random_value() >> 24;
            ((Uint8 *)reference->pixels)[i] = ((Uint8 *)actual->pixels)[i] = initial[i];
        }
        SDL_SetAlpha(source, am < 0 ? 0 : SDL_SRCALPHA, am < 0 ? 255 : alphas[am]);
        SDL_SetColorKey(source, key ? SDL_SRCCOLORKEY : 0,
                        SDL_C16LoadPixel(source->pixels, source->format->BytesPerPixel));
        if (SDL_BlitSurface(source, &sr, reference, &rr) < 0) {
            /* Upstream explicitly rejects indexed->indexed alpha blits. */
            SDL_FreeSurface(source); SDL_FreeSurface(reference); SDL_FreeSurface(actual);
            continue;
        }
        if (SDL_MapSurface(source, actual) < 0) return 2;
        source->map->sw_data->blit = SDL_C16CalculateBlit(source);
        if (!source->map->sw_data->blit || SDL_BlitSurface(source, &sr, actual, &ar) < 0) return 2;
        pixel_alpha = am >= 0 && source->format->Amask != 0;
        for (y = 0; y < actual->h; ++y) for (x = 0; x < actual->w; ++x) {
            SDL_PixelFormat *format = actual->format;
            long offset = (long)y * actual->pitch + x * format->BytesPerPixel;
            Uint32 expected = SDL_C16LoadPixel((Uint8 *)reference->pixels + offset, format->BytesPerPixel);
            Uint32 got = SDL_C16LoadPixel((Uint8 *)actual->pixels + offset, format->BytesPerPixel);
            Uint32 original = SDL_C16LoadPixel(initial + offset, format->BytesPerPixel);
            Uint32 rgb = format->palette ? 255 : format->Rmask | format->Gmask | format->Bmask;
            if ((expected & rgb) != (got & rgb)) bad = 1;
            if (pixel_alpha && format->Amask && (got & format->Amask) != (original & format->Amask)) bad = 1;
            if ((expected & format->Amask) != (got & format->Amask)) {
                /* SDL 1.2's generic unsigned ALPHA_BLEND can overflow into
                 * destination alpha when converting channel orders. Preserve
                 * destination alpha as documented instead of copying that bug. */
                if (pixel_alpha && (got & format->Amask) == (original & format->Amask)) alpha_correction = 1;
                else bad = 1;
            }
            if ((expected ^ got) & ~(rgb | format->Amask)) unused = 1;
        }
        if (bad) {
            fprintf(stderr, "pixel mismatch src=%d dst=%d alpha=%d key=%d\n",
                    si, di, am < 0 ? -1 : (int)alphas[am], key);
            ++failures;
        }
        cases += 1; unused_cases += unused; alpha_correction_cases += alpha_correction;
        SDL_FreeSurface(source); SDL_FreeSurface(reference); SDL_FreeSurface(actual);
    }
    printf("PIXEL_MATRIX cases=%d failures=%d unused_bit_differences=%d alpha_corrections=%d\n",
           cases, failures, unused_cases, alpha_correction_cases);
    return failures ? 1 : 0;
}
