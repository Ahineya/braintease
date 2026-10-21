/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NETSURF_IMAGE_LIMITS_H
#define NETSURF_IMAGE_LIMITS_H
#include <stdlib.h>
#include <errno.h>
/* Bounded overrides. Zero reports invalid configuration to the caller. */
static unsigned long ns_image_limit(const char *name,unsigned long fallback,unsigned long ceiling) {
    const char *text=getenv(name);char *end;unsigned long value;
    if(!text)return fallback;
    if(*text<'1'||*text>'9')return 0;
    errno=0;value=strtoul(text,&end,10);
    return errno||*end||!value||value>ceiling?0:value;
}
#ifdef RIPPLE_NETSURF_HOSTED
#define NS_IMAGE_CACHE_DEFAULT 8388608UL
#define NS_IMAGE_PIXELS_DEFAULT 1048576UL
#define NS_IMAGE_INPUT_DEFAULT 8388608UL
#define NS_IMAGE_WORK_DEFAULT 16777216UL
#define NS_IMAGE_SIDE 2048
#else
#define NS_IMAGE_CACHE_DEFAULT 65536UL
#define NS_IMAGE_PIXELS_DEFAULT 65536UL
#define NS_IMAGE_INPUT_DEFAULT 262144UL
#define NS_IMAGE_WORK_DEFAULT 393216UL
#define NS_IMAGE_SIDE 512
#endif
#endif
