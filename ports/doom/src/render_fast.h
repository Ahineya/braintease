#ifndef DOOM_RENDER_FAST_H
#define DOOM_RENDER_FAST_H
#include <stdint.h>

/* These leaf routines implement the ordinary C loops with the existing VM
 * instruction set. Pointers may cross banks; count is in 16-bit C bytes.
 * The port builds them only for Ripple, retaining upstream C for the native
 * reference. The framebuffer and patch-column stride is always 320. */
void Doom_ConvertRGB565(unsigned short *dest, const unsigned char *source,
                       const unsigned short *palette, unsigned int count);
void Doom_CopyPatchColumn(unsigned char *dest, const unsigned char *source,
                          unsigned int count);
void Doom_DrawColumn(unsigned char *dest, const unsigned char *source,
                     const unsigned char *map, uint32_t frac, uint32_t step,
                     unsigned int count);
void Doom_DrawSpan(unsigned char *dest, const unsigned char *source,
                   const unsigned char *map, uint32_t position, uint32_t step,
                   unsigned int count);
/* Low detail duplicates samples into two adjacent output cells. Count is
 * samples, not output cells; columns step by 320 and spans by two cells. */
void Doom_DrawColumnLow(unsigned char *dest, const unsigned char *source,
                        const unsigned char *map, uint32_t frac, uint32_t step,
                        unsigned int count);
void Doom_DrawSpanLow(unsigned char *dest, const unsigned char *source,
                      const unsigned char *map, uint32_t position, uint32_t step,
                      unsigned int count);
#endif
