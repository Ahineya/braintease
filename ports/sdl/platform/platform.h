/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RIPLIX_SDL_PLATFORM_H
#define RIPLIX_SDL_PLATFORM_H

#include "SDL_stdinc.h"

/* OS services only. Pixel representation adaptations belong in the SDL
 * software renderer, not in this interface. These functions do not retain
 * any borrowed GX buffer or enter SDL callbacks while sleeping. */
void SDL_GX_StartTicks(void);
Uint32 SDL_GX_GetTicks(void);
void SDL_GX_Delay(Uint32 milliseconds);
/* Returns 1 for pending graphics input, 0 for timeout, or -1 on failure. */
int SDL_GX_Wait(Uint32 milliseconds);

#endif
