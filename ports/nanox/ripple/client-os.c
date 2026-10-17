/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include "nano-X.h"
int GdError(const char *format,...){int n;va_list args;va_start(args,format);n=vfprintf(stderr,format,args);va_end(args);return n;}
MWTIMEOUT GdGetTickCount(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t)<0)return 0;return t.tv_sec*1000UL+t.tv_nsec/1000000UL;}
