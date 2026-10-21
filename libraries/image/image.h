/* SPDX-License-Identifier: MIT */
#ifndef RIPPLE_IMAGE_H
#define RIPPLE_IMAGE_H
#include <stddef.h>
#include <stdint.h>
/* Protocol octets occupy one addressable cell, on hosts and on C16 alike. */
typedef unsigned char RiByte;
typedef struct RiImage {
    unsigned width, height;
    RiByte *rgba; /* Straight (not premultiplied) RGBA, four octet cells/pixel. */
    unsigned opaque;
    unsigned source_width, source_height;
} RiImage;
typedef struct RiLimits {
    unsigned width, height;
    uint32_t pixels, input, work; /* work counts temporary C cells, not octets. */
} RiLimits;
#define RI_OK 0
#define RI_FORMAT (-1)
#define RI_CORRUPT (-2)
#define RI_LIMIT (-3)
#define RI_MEMORY (-4)
#define RI_UNSUPPORTED (-5)
/* Default: dimensions <=512, <=65536 pixels, <=256 Ki octets input,
 * <=768 Ki temporary cells. Every failure leaves *out NULL. No I/O/globals.
 * GIF returns its first displayed frame; JPEG supports baseline and progressive.
 * Returned RGBA and RiImage are owned together; release with ri_image_free. */
int ri_image_decode(const RiByte *, size_t, const RiLimits *, RiImage **out);
/* JPEG may reduce while decoding to fit output dimensions/pixels. Other codecs
 * retain strict output limits. Source and output budgets are independent;
 * source_width/source_height retain intrinsic layout dimensions. */
int ri_image_decode_fit(const RiByte *, size_t, const RiLimits *source,
    const RiLimits *output, RiImage **out);
void ri_image_free(RiImage *);
const char *ri_image_error(int);
/* Owned zeroed canvas, for image consumers such as browser bitmap bridges. */
RiImage *ri_image_new(unsigned width, unsigned height, const RiLimits *);
/* RGB565 conversion and source-over blending. These
 * helpers do not depend on a window server, filesystem or browser. */
uint16_t ri_image_rgb565(const RiByte *rgba);
uint16_t ri_image_over565(uint16_t foreground, unsigned alpha, uint16_t background);
#endif
