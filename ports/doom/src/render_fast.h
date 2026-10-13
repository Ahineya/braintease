#ifndef DOOM_RENDER_FAST_H
#define DOOM_RENDER_FAST_H
#include <stdint.h>

/* These leaf routines implement the ordinary C loops with the existing VM
 * instruction set. Pointers may cross banks; count is in 16-bit C bytes.
 * The port builds them only for Ripple, retaining upstream C for the native
 * reference. The framebuffer and patch-column stride is always 320. */
void Doom_ConvertRGB565(unsigned short *dest, const unsigned char *source,
                       const unsigned short *palette, unsigned int count);
/* The Doom framebuffer contains decoded palette indices in 0..255. This
 * variant avoids remasking every cell; callers must preserve that invariant. */
void Doom_ConvertRGB565Octets(unsigned short *dest, const unsigned char *source,
                             const unsigned short *palette, unsigned int count);
/* Pack a sampled row of decoded indices. Count <= 320; scale is 1, 2 or 4.
 * Source advances by scale cells, destination by one. */
void Doom_ConvertRGB565SampledRow(unsigned short *dest, const unsigned char *source,
                                 const unsigned short *palette, unsigned int count,
                                 unsigned int scale);
void Doom_CopyPatchColumn(unsigned char *dest, const unsigned char *source,
                          unsigned int count);
/* Count is retained samples. Source advances by scale, destination by
 * 320*scale; scale is 1, 2 or 4. Both pointers may cross banks. */
void Doom_CopyPatchColumnSampled(unsigned char *dest, const unsigned char *source,
                                 unsigned int count, unsigned int scale);
/* Copy retained cells between non-overlapping sparse rows (or identical
 * pointers), advancing both pointers by scale cells per sample. */
void Doom_CopySamples(unsigned char *dest, const unsigned char *source,
                       unsigned int count, unsigned int scale);
void Doom_DrawColumn(unsigned char *dest, const unsigned char *source,
                     const unsigned char *map, uint32_t frac, uint32_t step,
                     unsigned int count);
void Doom_DrawSpan(unsigned char *dest, const unsigned char *source,
                   const unsigned char *map, uint32_t position, uint32_t step,
                   unsigned int count);
/* Sparse raster output: one store per selected sample. Caller aligns the
 * starting coordinate and scales the texture step; stride is 1..1280 cells. */
void Doom_DrawColumnStrided(unsigned char *dest, const unsigned char *source,
                            const unsigned char *map, uint32_t frac, uint32_t step,
                            unsigned int count, unsigned int stride);
void Doom_DrawSpanStrided(unsigned char *dest, const unsigned char *source,
                          const unsigned char *map, uint32_t position, uint32_t step,
                          unsigned int count, unsigned int stride);
/* Low detail duplicates samples into two adjacent output cells. Count is
 * samples, not output cells; columns step by 320 and spans by two cells. */
void Doom_DrawColumnLow(unsigned char *dest, const unsigned char *source,
                        const unsigned char *map, uint32_t frac, uint32_t step,
                        unsigned int count);
void Doom_DrawSpanLow(unsigned char *dest, const unsigned char *source,
                      const unsigned char *map, uint32_t position, uint32_t step,
                      unsigned int count);
#endif
