/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RIPLIX_SDL_AUDIO_H
#define RIPLIX_SDL_AUDIO_H
#include "SDL_audio.h"

/* Cooperative service boundaries. Guest callbacks never run in a host audio
 * thread, from a signal handler, or while SDL_LockAudio holds the mixer. */
void SDL_C16AudioPump(void);
Uint32 SDL_C16AudioWait(Uint32 maximum);
Uint16 SDL_C16AudioMixFormat(void);

/* A device consumes copied, interleaved S16 sample cells. SDL callback and
 * conversion lengths remain logical octets, independent of sizeof(Sint16). */
int SDL_PCMOpen(Sint32 rate, Uint8 channels, Uint32 capacity);
void SDL_PCMClose(void);
int SDL_PCMState(Uint32 *queued, Uint32 *capacity);
Sint32 SDL_PCMWrite(const Sint16 *samples, Uint32 frames);
int SDL_PCMStart(void);
int SDL_PCMPause(void);
#endif
