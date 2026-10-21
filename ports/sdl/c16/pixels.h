/* Logical-octet pixel storage for the C16 SDL software renderer. */
#ifndef SDL_C16_PIXELS_H
#define SDL_C16_PIXELS_H

#include "SDL_video.h"
#include "SDL_blit.h"

/* A surface's pitch and BytesPerPixel count octets, each held in one Uint8
 * carrier. They do not count sizeof(Uint16) or sizeof(Uint32) storage units.
 * These helpers also work on byte-addressed reference builds. */
extern Uint32 SDL_C16LoadPixel(const Uint8 *pixels, int octets);
extern void SDL_C16StorePixel(Uint8 *pixels, int octets, Uint32 pixel);

/* Installs the source surface as the low-level blitter's aux_data. The normal
 * SDL software wrapper still performs clipping, locking and map validation. */
extern SDL_loblit SDL_C16CalculateBlit(SDL_Surface *surface);

/* Complete software replacement: clips dstrect in place and locks the surface.
 * Packed 1/4-bpp surfaces are deliberately unsupported. */
extern int SDL_C16FillRect(SDL_Surface *dst, SDL_Rect *dstrect, Uint32 color);

#endif
