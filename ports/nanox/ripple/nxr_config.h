/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NANOX_RIPPLE_CONFIG_H
#define NANOX_RIPPLE_CONFIG_H
/* C16 engine profile. Raster addresses and pitches count cells, not octets. */
#define MWPIXEL_FORMAT MWPF_TRUECOLOR565
#define MW_CPU_LITTLE_ENDIAN 1
#define MW_CPU_BIG_ENDIAN 0
#define UNIX 1
#define USE_ALLOCA 0
#define DYNAMICREGIONS 1
#define POLYREGIONS 0
#define MW_FEATURE_PALETTE 0
#define MW_FEATURE_PORTRAIT 0
#define MW_FEATURE_AREAS 0
#define MW_FEATURE_IMAGES 0
#define MW_FEATURE_SHAPES 0
#define MW_FEATURE_INTL 0
#if NXR_DESKTOP
#define MW_FEATURE_TIMERS 1
#else
#define MW_FEATURE_TIMERS 0
#endif
#define AUTO_START_SERVER 0
#define NONETWORK 0
#define NANOWM 0
#define NUKLEARUI 1
#define HAVE_FLOAT 0
#define HAVE_MMAP 0
#define HAVE_SHAREDMEM_SUPPORT 0
#define HAVE_FILEIO 0
#endif
