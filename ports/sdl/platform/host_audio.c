/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Reference runner only: a clocked null sink, with no host speaker access. */
#include "SDL.h"
#include "audio.h"
#include "platform.h"
static Uint32 count, limit, rate, last, fraction;
static int running;
static void advance(void) {
    Uint32 now = SDL_GX_GetTicks(), elapsed = now - last, consumed;
    last = now;
    if (!running) return;
    if (elapsed > 2000) elapsed = 2000;
    consumed = elapsed * rate + fraction;
    fraction = consumed % 1000UL;
    consumed /= 1000UL;
    count = consumed < count ? count - consumed : 0;
}
int SDL_PCMOpen(Sint32 frequency, Uint8 channels, Uint32 capacity) {
    (void)channels;
    rate = frequency;
    limit = capacity;
    count = fraction = 0;
    running = 0;
    last = SDL_GX_GetTicks();
    return 0;
}
void SDL_PCMClose(void) { count = limit = running = 0; }
int SDL_PCMState(Uint32 *queued, Uint32 *capacity) {
    advance();
    *queued = count;
    *capacity = limit;
    return 0;
}
Sint32 SDL_PCMWrite(const Sint16 *samples, Uint32 frames) {
    (void)samples;
    advance();
    if (frames > limit - count) frames = limit - count;
    if (frames > 1024) frames = 1024;
    count += frames;
    return frames;
}
int SDL_PCMStart(void) { last = SDL_GX_GetTicks(); running = 1; return 0; }
int SDL_PCMPause(void) { advance(); running = 0; return 0; }
