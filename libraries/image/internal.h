/* SPDX-License-Identifier: MIT */
#ifndef RIPPLE_IMAGE_INTERNAL_H
#define RIPPLE_IMAGE_INTERNAL_H
#include "image.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>
extern const RiLimits ri_defaults;
uint32_t ri_be32(const RiByte *);
uint32_t ri_le32(const RiByte *);
unsigned ri_le16(const RiByte *);
int ri_size(unsigned, unsigned, const RiLimits *);
int ri_png(const RiByte *, size_t, const RiLimits *, RiImage **);
int ri_gif(const RiByte *, size_t, const RiLimits *, RiImage **);
int ri_bmp(const RiByte *, size_t, const RiLimits *, RiImage **);
int ri_jpeg_fit(const RiByte *, size_t, const RiLimits *, const RiLimits *, RiImage **);
int ri_jpeg(const RiByte *, size_t, const RiLimits *, RiImage **);
#endif
