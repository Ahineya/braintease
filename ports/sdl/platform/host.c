/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Reference tests only: no production host window or hardware assumptions. */
#include "SDL.h"
#include <time.h>
#include <sys/select.h>
#include <errno.h>
Uint32 SDL_GX_GetTicks(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (Uint32)t.tv_sec*1000U+(Uint32)t.tv_nsec/1000000U;
}
void SDL_GX_Delay(Uint32 ms) {
    struct timeval t;
    t.tv_sec=ms/1000;t.tv_usec=(ms%1000)*1000;
    while(select(0,NULL,NULL,NULL,&t)<0 && errno==EINTR){}
}
