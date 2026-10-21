/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* PCM octets use Uint8 carriers, including when CHAR_BIT is 16. */
#ifndef SDL_C16_AUDIO_FORMATS_H
#define SDL_C16_AUDIO_FORMATS_H
#include "SDL_audio.h"

extern int SDL_C16AudioFormatValid(Uint16 format);
extern Sint32 SDL_C16AudioReadSample(const Uint8 *src, Uint16 format);
extern void SDL_C16AudioWriteSample(Uint8 *dst, Uint16 format, Sint32 sample);
extern void SDL_C16MixAudioFormat(Uint8 *dst, const Uint8 *src,
                                Uint32 len, Uint16 format, int volume);
/* Lifecycle supplies the callback's format, or zero when no device is open. */
extern Uint16 SDL_C16AudioMixFormat(void);
#endif
