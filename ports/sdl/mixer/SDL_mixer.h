/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Original, bounded PCM/WAV effects profile of the SDL_mixer 1.2 source API.
 * This is not an upstream SDL_mixer release. See README.md for its boundaries.
 */
#ifndef SDL_C16_PCM_MIXER_H
#define SDL_C16_PCM_MIXER_H
#include "SDL.h"
#include "begin_code.h"
#ifdef __cplusplus
extern "C" {
#endif

#define SDL_MIXER_PCM_PROFILE 1
#define MIX_CHANNELS 8
#define MIX_MAX_VOLUME 128
#define MIX_DEFAULT_FREQUENCY 22050L
#define MIX_DEFAULT_FORMAT AUDIO_S16SYS
#define MIX_DEFAULT_CHANNELS 2
/* Public channel/count parameters retain the guest's signed 16-bit int ABI. */
#define MIX_PCM_MAX_CHANNELS 32767

typedef struct Mix_Chunk {
    int allocated;
    Uint8 *abuf;
    Uint32 alen;                 /* Logical octets, including on C16. */
    Uint8 volume;                /* 0..MIX_MAX_VOLUME. */
} Mix_Chunk;

typedef enum {
    MIX_NO_FADING, MIX_FADING_OUT, MIX_FADING_IN
} Mix_Fading;

extern DECLSPEC int SDLCALL Mix_OpenAudio(Sint32 frequency, Uint16 format,
                                         int channels, int chunksize);
extern DECLSPEC void SDLCALL Mix_CloseAudio(void);
extern DECLSPEC int SDLCALL Mix_QuerySpec(Sint32 *frequency, Uint16 *format,
                                         int *channels);
extern DECLSPEC int SDLCALL Mix_AllocateChannels(int channels);
extern DECLSPEC int SDLCALL Mix_ReserveChannels(int channels);
extern DECLSPEC Mix_Chunk * SDLCALL Mix_LoadWAV_RW(SDL_RWops *src, int freesrc);
#define Mix_LoadWAV(file) Mix_LoadWAV_RW(SDL_RWFromFile((file), "rb"), 1)
/* RAW storage is borrowed. It must remain valid and unchanged until freed or
 * stopped. Format, rate and channels must match Mix_QuerySpec's device. */
extern DECLSPEC Mix_Chunk * SDLCALL Mix_QuickLoad_RAW(Uint8 *mem, Uint32 len);
extern DECLSPEC void SDLCALL Mix_FreeChunk(Mix_Chunk *chunk);
extern DECLSPEC int SDLCALL Mix_PlayChannelTimed(int channel, Mix_Chunk *chunk,
                                               int loops, Sint32 ticks);
#define Mix_PlayChannel(channel, chunk, loops) \
    Mix_PlayChannelTimed((channel), (chunk), (loops), -1)
extern DECLSPEC int SDLCALL Mix_FadeInChannelTimed(int channel, Mix_Chunk *chunk,
                                                 int loops, Sint32 ms, Sint32 ticks);
#define Mix_FadeInChannel(channel, chunk, loops, ms) \
    Mix_FadeInChannelTimed((channel), (chunk), (loops), (ms), -1)
extern DECLSPEC int SDLCALL Mix_HaltChannel(int channel);
extern DECLSPEC int SDLCALL Mix_ExpireChannel(int channel, Sint32 ticks);
extern DECLSPEC int SDLCALL Mix_Volume(int channel, int volume);
extern DECLSPEC int SDLCALL Mix_VolumeChunk(Mix_Chunk *chunk, int volume);
extern DECLSPEC void SDLCALL Mix_Pause(int channel);
extern DECLSPEC void SDLCALL Mix_Resume(int channel);
extern DECLSPEC int SDLCALL Mix_Paused(int channel);
extern DECLSPEC int SDLCALL Mix_Playing(int channel);
extern DECLSPEC int SDLCALL Mix_FadeOutChannel(int channel, Sint32 ms);
extern DECLSPEC Mix_Fading SDLCALL Mix_FadingChannel(int channel);
extern DECLSPEC void SDLCALL Mix_ChannelFinished(void (SDLCALL *callback)(int));
extern DECLSPEC Mix_Chunk * SDLCALL Mix_GetChunk(int channel);
#define Mix_GetError SDL_GetError
#define Mix_SetError SDL_SetError

/* Music and compressed decoders are explicitly outside this profile. These
 * common music entry points report that boundary instead of claiming support. */
typedef struct Mix_Music Mix_Music;
extern DECLSPEC Mix_Music * SDLCALL Mix_LoadMUS(const char *file);
extern DECLSPEC Mix_Music * SDLCALL Mix_LoadMUS_RW(SDL_RWops *src);
extern DECLSPEC int SDLCALL Mix_PlayMusic(Mix_Music *music, int loops);
extern DECLSPEC int SDLCALL Mix_FadeInMusic(Mix_Music *music, int loops, Sint32 ms);
extern DECLSPEC int SDLCALL Mix_HaltMusic(void);
extern DECLSPEC int SDLCALL Mix_PlayingMusic(void);
extern DECLSPEC void SDLCALL Mix_FreeMusic(Mix_Music *music);

#ifdef __cplusplus
}
#endif
#include "close_code.h"
#endif
