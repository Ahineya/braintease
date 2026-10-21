/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* SDL_mixer effects tests. MIXER_FAKE_AUDIO drives the real production mixer
 * callback deterministically; the ordinary executable uses real /dev/pcm. */
#include "SDL.h"
#include "SDL_mixer.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

static unsigned checks;
#define CHECK(c) do { ++checks; if (!(c)) { fprintf(stderr, "SDL_MIXER_FAIL line %d: %s (%s)\n", __LINE__, #c, SDL_GetError()); exit(1); } } while (0)

static void put16(Uint8 *p, Sint32 v)
{
    Uint32 n = (Uint32)v & 65535UL;
    p[0] = (Uint8)(n & 255U); p[1] = (Uint8)(n >> 8);
}
static Sint32 get16(const Uint8 *p)
{
    Uint32 n = (Uint32)(p[0] & 255U) | ((Uint32)(p[1] & 255U) << 8);
    return n >= 32768UL ? (Sint32)n - 65536L : (Sint32)n;
}
static void put32(Uint8 *p, Uint32 n)
{
    put16(p, n & 65535UL); put16(p + 2, n >> 16);
}
static void four(Uint8 *p, const char *s)
{
    unsigned i; for (i = 0; i < 4; ++i) p[i] = (Uint8)s[i];
}
static Uint32 wave_data(Uint8 *p, Uint32 frames, unsigned channels,
                       unsigned bits, Sint32 rate, int ancillary)
{
    Uint32 at = 12, bytes = frames * channels * (bits / 8), i;
    four(p, "RIFF"); four(p + 8, "WAVE");
    if (ancillary) {
        four(p + at, "JUNK"); put32(p + at + 4, 3);
        p[at + 8] = 7; p[at + 9] = 8; p[at + 10] = 9; p[at + 11] = 0; at += 12;
    }
    four(p + at, "fmt "); put32(p + at + 4, 16);
    put16(p + at + 8, 1); put16(p + at + 10, channels);
    put32(p + at + 12, rate); put32(p + at + 16, (Uint32)rate * channels * (bits / 8));
    put16(p + at + 20, channels * (bits / 8)); put16(p + at + 22, bits); at += 24;
    four(p + at, "data"); put32(p + at + 4, bytes); at += 8;
    for (i = 0; i < bytes; ++i) p[at + i] = (Uint8)((i * 37UL + 19UL) & 255U);
    if (bytes & 1U) p[at + bytes] = 0;
    at += bytes + (bytes & 1U); put32(p + 4, at - 8); return at;
}
static Mix_Chunk *raw_chunk(Uint8 *p, const Sint32 *values, unsigned frames)
{
    unsigned i;
    for (i = 0; i < frames * 2U; ++i) put16(p + i * 2U, values[i]);
    return Mix_QuickLoad_RAW(p, (Uint32)frames * 4UL);
}
static void constant(Uint8 *p, unsigned frames, Sint32 value)
{
    unsigned i; for (i = 0; i < frames * 2U; ++i) put16(p + i * 2U, value);
}

#ifdef MIXER_FAKE_AUDIO
/* Deliberately minimal SDL audio service: only the selected production mixer
 * callback is faked. WAV parsing and PCM conversion use the real SDL code. */
static SDL_AudioSpec fake_spec;
static unsigned fake_live, fake_paused, fake_locks, fake_calls, fake_closes;
static char error_text[256];
void SDL_SetError(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vsnprintf(error_text, sizeof(error_text), fmt, ap); va_end(ap);
}
void SDL_Error(SDL_errorcode code) { SDL_SetError("SDL error %d", (int)code); }
char *SDL_GetError(void) { return error_text; }
void SDL_ClearError(void) { error_text[0] = 0; }
int SDL_InitSubSystem(Uint32 flags) { (void)flags; return 0; }
int SDL_Init(Uint32 flags) { return SDL_InitSubSystem(flags); }
void SDL_Quit(void) { }
Uint32 SDL_WasInit(Uint32 flags) { return flags & SDL_INIT_AUDIO; }
Uint32 SDL_GetTicks(void) { return 0; }
void SDL_Delay(Uint32 ticks) { (void)ticks; }
int SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained)
{
    if (fake_live) { SDL_SetError("Audio already open"); return -1; }
    CHECK(desired && desired->callback && desired->freq > 0);
    fake_spec = *desired;
    fake_spec.size = (Uint32)desired->samples * desired->channels * ((desired->format & 255U) / 8U);
    fake_spec.silence = desired->format == AUDIO_U8 ? 128 : 0;
    *desired = fake_spec; if (obtained) *obtained = fake_spec;
    fake_live = 1; fake_paused = 1; return 0;
}
void SDL_CloseAudio(void) { if (fake_live) ++fake_closes; fake_live = 0; }
void SDL_PauseAudio(int pause) { fake_paused = pause != 0; }
void SDL_LockAudio(void) { ++fake_locks; }
void SDL_UnlockAudio(void) { CHECK(fake_locks > 0); --fake_locks; }
SDL_audiostatus SDL_GetAudioStatus(void)
{
    return !fake_live ? SDL_AUDIO_STOPPED : fake_paused ? SDL_AUDIO_PAUSED : SDL_AUDIO_PLAYING;
}
Uint16 SDL_C16AudioMixFormat(void) { return fake_live ? fake_spec.format : 0; }
/* Elementwise copies model a logical octet stream even when Uint8 is wider. */
static unsigned rw_closed;
#if SDL_RIPLIX_C16
typedef Sint32 MixerRWCount;
#else
typedef int MixerRWCount;
#endif
static MixerRWCount test_read(SDL_RWops *r, void *dst, MixerRWCount size, MixerRWCount count)
{
    Uint32 bytes, i, available = r->hidden.mem.stop - r->hidden.mem.here;
    if (size <= 0 || count <= 0) return 0;
    bytes = (Uint32)size * (Uint32)count;
    if (bytes > available) bytes = available - available % (unsigned)size;
    for (i = 0; i < bytes; ++i) ((Uint8 *)dst)[i] = r->hidden.mem.here[i];
    r->hidden.mem.here += bytes; return (MixerRWCount)(bytes / (Uint32)size);
}
static MixerRWCount test_seek(SDL_RWops *r, MixerRWCount off, MixerRWCount whence)
{
    MixerRWCount length = (MixerRWCount)(r->hidden.mem.stop - r->hidden.mem.base);
    MixerRWCount pos = whence == RW_SEEK_SET ? 0 : whence == RW_SEEK_END ? length : (MixerRWCount)(r->hidden.mem.here - r->hidden.mem.base);
    pos += off; if (pos < 0) pos = 0; if (pos > length) pos = length;
    r->hidden.mem.here = r->hidden.mem.base + pos; return pos;
}
static MixerRWCount test_close(SDL_RWops *r) { ++rw_closed; free(r); return 0; }
SDL_RWops *SDL_RWFromConstMem(const void *p, MixerRWCount length)
{
    SDL_RWops *r = (SDL_RWops *)malloc(sizeof(*r)); CHECK(r && p && length >= 0);
    memset(r, 0, sizeof(*r)); r->read = test_read; r->seek = test_seek; r->close = test_close;
    r->hidden.mem.base = r->hidden.mem.here = (Uint8 *)p; r->hidden.mem.stop = (Uint8 *)p + length; return r;
}
SDL_RWops *SDL_RWFromFile(const char *name, const char *mode)
{
    (void)name; (void)mode; SDL_SetError("No file in deterministic backend"); return NULL;
}
static void render(Uint8 *out, unsigned frames)
{
    unsigned i, length = frames * fake_spec.channels * ((fake_spec.format & 255U) / 8U);
    CHECK(fake_live && length < 32768U);
    for (i = 0; i < length; ++i) out[i] = 0xa5;
    if (fake_paused || fake_locks) {
        for (i = 0; i < length; ++i) out[i] = 0;
        return;
    }
    ++fake_calls; ++fake_locks; fake_spec.callback(fake_spec.userdata, out, (int)length); --fake_locks;
    for (i = 0; i < length; ++i) CHECK(out[i] <= 255U);
    CHECK(!fake_locks);
}
static void frame(Uint8 *out, unsigned at, Sint32 left, Sint32 right)
{
    CHECK(get16(out + at * 4U) == left); CHECK(get16(out + at * 4U + 2U) == right);
}
static void write_pcm(Uint8 *p, Uint16 fmt, Sint32 sample)
{
    Uint32 value;
    unsigned bytes = (fmt & 255U) / 8U;
    if (!(fmt & 0x8000U)) sample += bytes == 1 ? 128 : 32768L;
    value = (Uint32)sample & (bytes == 1 ? 255UL : 65535UL);
    if (bytes == 1) p[0] = (Uint8)value;
    else if (fmt & 0x1000U) { p[0] = (Uint8)(value >> 8); p[1] = (Uint8)(value & 255U); }
    else { p[0] = (Uint8)(value & 255U); p[1] = (Uint8)(value >> 8); }
}
static Sint32 read_pcm(const Uint8 *p, Uint16 fmt)
{
    unsigned bytes = (fmt & 255U) / 8U;
    Uint32 value = p[0] & 255U;
    Sint32 sample;
    if (bytes == 2) value = fmt & 0x1000U ? (value << 8) | (p[1] & 255U) : value | ((Uint32)(p[1] & 255U) << 8);
    sample = (Sint32)value;
    if (fmt & 0x8000U) {
        if (value >= (bytes == 1 ? 128UL : 32768UL)) sample -= bytes == 1 ? 256L : 65536L;
    } else sample -= bytes == 1 ? 128 : 32768L;
    return sample;
}
static unsigned finished_count, finished_mask;
static Mix_Chunk *finished_free;
static void finished(int channel)
{
    CHECK(fake_locks > 0); CHECK(channel >= 0 && channel < 8);
    ++finished_count; finished_mask |= 1U << channel;
    /* Querying and updating from the callback must preserve nested locking. */
    if (channel < Mix_AllocateChannels(-1)) CHECK(Mix_Volume(channel, -1) >= 0);
    if (finished_free) {
        Mix_Chunk *victim = finished_free;
        finished_free = NULL;
        Mix_FreeChunk(victim);
    }
}
static void deterministic(void)
{
    Uint8 a[128], b[128], c[128], out[256], wav[256];
    Sint32 values[8] = { 4000, -4000, 8000, -8000, 12000, -12000, 16000, -16000 };
    Mix_Chunk *first, *second, *third, *loaded;
    SDL_RWops *rw;
    Sint32 rate; Uint16 fmt; int channels, i, before; Uint32 length;
    CHECK(Mix_QuerySpec(NULL, NULL, NULL) == 0);
    CHECK(Mix_PlayChannel(-1, NULL, 0) < 0 && *Mix_GetError());
    CHECK(Mix_OpenAudio(123, AUDIO_S16LSB, 2, 8) < 0 && !fake_live);
    CHECK(Mix_OpenAudio(8000, AUDIO_S16LSB, 2, 8192) < 0 && !fake_live);
    CHECK(Mix_OpenAudio(8000, AUDIO_S16LSB, 2, 8) == 0);
    CHECK(Mix_QuerySpec(&rate, &fmt, &channels) > 0 && rate == 8000 && fmt == AUDIO_S16LSB && channels == 2);
    CHECK(Mix_AllocateChannels(-1) == 8); CHECK(Mix_AllocateChannels(4) == 4);
    CHECK(Mix_ReserveChannels(2) == 2);
    first = raw_chunk(a, values, 4); CHECK(first && first->alen == 16 && first->abuf == a);
    constant(b, 8, 24000); second = Mix_QuickLoad_RAW(b, 32); CHECK(second);
    constant(c, 8, -16000); third = Mix_QuickLoad_RAW(c, 32); CHECK(third);
    CHECK(Mix_AllocateChannels(257) == 257 && Mix_ReserveChannels(256) == 256);
    CHECK(Mix_PlayChannel(-1, first, -1) == 256 && Mix_Playing(256));
    Mix_HaltChannel(-1); CHECK(Mix_AllocateChannels(4) == 4 && Mix_ReserveChannels(2) == 2);
    CHECK(Mix_PlayChannel(-1, first, -1) == 2); CHECK(Mix_PlayChannel(-1, first, -1) == 3);
    CHECK(Mix_PlayChannel(-1, first, -1) < 0); CHECK(Mix_Playing(-1) == 2);
    CHECK(Mix_PlayChannel(0, first, -1) == 0 && Mix_GetChunk(0) == first);
    CHECK(Mix_PlayChannel(8, first, 0) < 0); CHECK(Mix_PlayChannel(1, first, -2) < 0);
    CHECK(Mix_HaltChannel(-1) == 0 && !Mix_Playing(-1)); CHECK(Mix_ReserveChannels(0) == 0);
    Mix_ChannelFinished(finished); finished_count = finished_mask = 0;

    /* Accumulate all three voices before clipping; channel order must not
     * change 24000 + 24000 - 16000 into a prematurely saturated result. */
    CHECK(Mix_PlayChannel(0, second, -1) == 0); CHECK(Mix_PlayChannel(1, second, -1) == 1);
    CHECK(Mix_PlayChannel(2, third, -1) == 2); render(out, 12);
    for (i = 0; i < 12; ++i) frame(out, i, 32000, 32000);
    Mix_HaltChannel(2); render(out, 4); for (i = 0; i < 4; ++i) frame(out, i, 32767, 32767);
    Mix_HaltChannel(-1); constant(b, 8, -24000); constant(c, 8, 16000);
    Mix_PlayChannel(0, second, -1); Mix_PlayChannel(1, second, -1); Mix_PlayChannel(2, third, -1);
    render(out, 4); for (i = 0; i < 4; ++i) frame(out, i, -32000, -32000);
    Mix_HaltChannel(2); render(out, 4); for (i = 0; i < 4; ++i) frame(out, i, -32768L, -32768L);
    Mix_HaltChannel(-1);

    CHECK(Mix_Volume(0, 64) == 128); CHECK(Mix_VolumeChunk(first, 64) == 128);
    CHECK(Mix_PlayChannel(0, first, 2) == 0); render(out, 10);
    for (i = 0; i < 10; ++i) frame(out, i, values[(i % 4) * 2] / 4, values[(i % 4) * 2 + 1] / 4);
    CHECK(Mix_Playing(0)); before = finished_count; render(out, 4);
    frame(out, 0, 3000, -3000); frame(out, 1, 4000, -4000); frame(out, 2, 0, 0); frame(out, 3, 0, 0);
    CHECK(!Mix_Playing(0) && finished_count == (unsigned)before + 1);
    CHECK(Mix_Volume(0, 128) == 64); CHECK(Mix_VolumeChunk(first, 128) == 64);
    CHECK(Mix_Volume(0, 999) == 128 && Mix_Volume(0, -1) == 128);
    Mix_PlayChannel(0, first, -1); render(out, 3); Mix_Pause(0);
    CHECK(Mix_Paused(0) == 1 && Mix_Playing(0) == 1); render(out, 5);
    for (i = 0; i < 5; ++i) frame(out, i, 0, 0);
    Mix_Resume(0); render(out, 2); frame(out, 0, 16000, -16000); frame(out, 1, 4000, -4000);
    Mix_HaltChannel(-1);

    constant(b, 8, 16000); CHECK(Mix_FadeInChannel(0, second, -1, 1) == 0);
    CHECK(Mix_FadingChannel(0) == MIX_FADING_IN); render(out, 9);
    for (i = 0; i < 9; ++i) frame(out, i, i < 8 ? i * 2000 : 16000, i < 8 ? i * 2000 : 16000);
    CHECK(Mix_FadingChannel(0) == MIX_NO_FADING);
    CHECK(Mix_FadeOutChannel(0, 1) == 1); render(out, 10);
    for (i = 0; i < 10; ++i) frame(out, i, i < 8 ? 16000 - i * 2000 : 0, i < 8 ? 16000 - i * 2000 : 0);
    CHECK(!Mix_Playing(0) && Mix_FadingChannel(0) == MIX_NO_FADING);
    CHECK(Mix_PlayChannelTimed(0, second, -1, 1) == 0); render(out, 3); Mix_Pause(0); render(out, 10);
    Mix_Resume(0); render(out, 7);
    for (i = 0; i < 7; ++i) frame(out, i, i < 5 ? 16000 : 0, i < 5 ? 16000 : 0);
    CHECK(!Mix_Playing(0)); Mix_PlayChannel(0, second, -1);
    CHECK(Mix_ExpireChannel(0, 1) == 1); CHECK(Mix_ExpireChannel(0, -1) == 1);
    render(out, 12); CHECK(Mix_Playing(0)); CHECK(Mix_ExpireChannel(0, 0) == 1 && !Mix_Playing(0));
    CHECK(Mix_PlayChannelTimed(0, second, -1, 40000L) == 0); render(out, 12);
    CHECK(Mix_Playing(0) && Mix_ExpireChannel(0, 40000L) == 1); render(out, 12); CHECK(Mix_Playing(0));
    CHECK(Mix_FadeOutChannel(0, 40000L) == 1); render(out, 12);
    CHECK(Mix_Playing(0) && Mix_FadingChannel(0) == MIX_FADING_OUT); Mix_HaltChannel(0);
    /* Replacement notifies after the new voice has been installed. Queries
     * from that notification must remain safe and see the current voice. */
    Mix_PlayChannel(0, second, -1); before = finished_count;
    CHECK(Mix_PlayChannel(0, first, -1) == 0 && finished_count == (unsigned)before + 1);
    CHECK(Mix_GetChunk(0) == first && Mix_Playing(0)); Mix_HaltChannel(0);

    /* A user audio lock freezes callback production through nested Mix API
     * updates; unlock retains the exact source position. */
    Mix_PlayChannel(0, first, -1); render(out, 1); SDL_LockAudio(); before = fake_calls;
    Mix_Volume(0, 64); Mix_Pause(0); render(out, 4); CHECK(fake_calls == (unsigned)before);
    Mix_Resume(0); SDL_UnlockAudio(); render(out, 1); frame(out, 0, 4000, -4000);
    Mix_HaltChannel(-1); Mix_Volume(0, 128);
    Mix_PlayChannel(3, first, -1); before = finished_count;
    CHECK(Mix_AllocateChannels(2) == 2 && !Mix_Playing(-1) && finished_count == (unsigned)before + 1);
    CHECK(Mix_AllocateChannels(4) == 4);
    Mix_PlayChannel(0, first, -1); Mix_PlayChannel(1, first, -1); before = finished_count;
    Mix_FreeChunk(first); CHECK(!Mix_Playing(-1) && finished_count == (unsigned)before + 2);
    CHECK(a[0] == 0xa0 && a[1] == 0x0f); /* Borrowed PCM was not freed or changed. */

    length = wave_data(wav, 4, 1, 8, 4000, 1);
    before = rw_closed; rw = SDL_RWFromConstMem(wav, (MixerRWCount)length); loaded = Mix_LoadWAV_RW(rw, 1);
    CHECK(loaded && rw_closed == (unsigned)before + 1 && loaded->alen == 32);
    CHECK(Mix_PlayChannel(0, loaded, 0) == 0); render(out, 8);
    for (i = 0; i < 8; ++i) frame(out, i, ((Sint32)wav[56 + i / 2] - 128) * 256,
                                                  ((Sint32)wav[56 + i / 2] - 128) * 256);
    Mix_FreeChunk(loaded);
    length = wave_data(wav, 4, 2, 16, 8000, 0);
    for (i = 0; i < (int)length; ++i) {
        before = rw_closed; SDL_ClearError(); rw = SDL_RWFromConstMem(wav, i);
        CHECK(Mix_LoadWAV_RW(rw, 1) == NULL && rw_closed == (unsigned)before + 1 && *Mix_GetError());
    }
    for (i = 0; i < 9; ++i) {
        length = wave_data(wav, 4, 2, 16, 8000, 0);
        switch (i) { case 0: wav[0] = 'X'; break; case 1: put16(wav + 20, 17); break;
          case 2: put16(wav + 22, 3); break; case 3: put32(wav + 24, 0); break;
          case 4: put32(wav + 28, 1); break; case 5: put16(wav + 32, 3); break;
          case 6: put16(wav + 34, 24); break; case 7: put32(wav + 4, 0xffffffffUL); break;
          case 8: put32(wav + 40, 0xffffffffUL); break; }
        rw = SDL_RWFromConstMem(wav, (MixerRWCount)length); CHECK(Mix_LoadWAV_RW(rw, 1) == NULL && *Mix_GetError());
    }
    /* A completion hook may free another live voice's chunk. The whole block
     * has rendered before hooks run, and nested notifications are flattened. */
    Mix_PlayChannel(0, second, 0); Mix_PlayChannel(1, third, -1);
    finished_free = third; before = finished_count; render(out, 12);
    CHECK(!finished_free && !Mix_Playing(-1) && finished_count == (unsigned)before + 2);
    for (i = 0; i < 12; ++i) frame(out, i, i < 8 ? 32000 : 16000, i < 8 ? 32000 : 16000);
    third = NULL;
    Mix_PlayChannel(0, second, -1); before = fake_closes; Mix_CloseAudio();
    CHECK(!fake_live && fake_closes == (unsigned)before + 1 && !fake_locks && !Mix_QuerySpec(NULL, NULL, NULL));
    Mix_CloseAudio(); Mix_FreeChunk(second); Mix_FreeChunk(third);
    CHECK(Mix_OpenAudio(48000L, AUDIO_S16LSB, 2, 8) == 0);
    CHECK(Mix_QuerySpec(&rate, &fmt, &channels) > 0 && rate == 48000L);
    {
        Uint8 *large = (Uint8 *)malloc(40044UL * sizeof(Uint8)); CHECK(large);
        length = wave_data(large, 10000UL, 2, 16, 48000L, 0);
        loaded = Mix_LoadWAV_RW(SDL_RWFromConstMem(large, (MixerRWCount)length), 1);
        CHECK(loaded && loaded->alen == 40000UL); Mix_FreeChunk(loaded); free(large);
    }
    Mix_CloseAudio();
    /* LGeneral's tracked.wav uses 10989 Hz. Verify the exact rational source
     * index, output frame rounding, odd data padding and stereo expansion. */
    CHECK(Mix_OpenAudio(22050L, AUDIO_S16LSB, 2, 8) == 0);
    length = wave_data(wav, 109, 1, 8, 10989L, 1);
    before = rw_closed; rw = SDL_RWFromConstMem(wav, (MixerRWCount)length);
    loaded = Mix_LoadWAV_RW(rw, 0);
    CHECK(loaded && rw_closed == (unsigned)before && loaded->alen == (109UL * 22050UL / 10989UL) * 4UL);
    CHECK(SDL_RWclose(rw) == 0 && rw_closed == (unsigned)before + 1);
    CHECK(Mix_PlayChannel(0, loaded, 0) == 0);
    {
        Uint32 output_frame = 0, total = loaded->alen / 4UL;
        unsigned take, side;
        while (output_frame < total) {
            take = total - output_frame > 32 ? 32 : (unsigned)(total - output_frame);
            render(out, take);
            for (side = 0; side < take; ++side) {
                Uint32 source_frame = (output_frame + side) * 10989UL / 22050UL;
                Sint32 sample = ((Sint32)wav[56 + source_frame] - 128) * 256;
                frame(out, side, sample, sample);
            }
            output_frame += take;
        }
    }
    CHECK(!Mix_Playing(0)); Mix_FreeChunk(loaded);
    CHECK(!Mix_LoadMUS("unsupported.ogg") && *Mix_GetError());
    CHECK(Mix_PlayMusic(NULL, 0) < 0 && *Mix_GetError());
#if !SDL_RIPLIX_C16
    /* The C16 ABI cannot represent 32768 as a positive channel argument. */
    CHECK(Mix_AllocateChannels(32768) < 0);
#else
    CHECK(Mix_AllocateChannels(-1) == 8);
#endif
    Mix_CloseAudio();
    {
        Uint16 formats[6] = { AUDIO_U8, AUDIO_S8, AUDIO_U16LSB, AUDIO_S16LSB, AUDIO_U16MSB, AUDIO_S16MSB };
        unsigned which, sides, bytes, at, side;
        Sint32 unit, sample, expected, minimum, maximum;
        for (which = 0; which < 6; ++which) for (sides = 1; sides <= 2; ++sides) {
            fmt = formats[which]; bytes = (fmt & 255U) / 8U;
            unit = bytes == 1 ? 100 : 24000; minimum = bytes == 1 ? -128 : -32768L; maximum = bytes == 1 ? 127 : 32767L;
            CHECK(Mix_OpenAudio(8000, fmt, sides, 8) == 0);
            for (at = 0; at < 4; ++at) for (side = 0; side < sides; ++side) {
                sample = (at + side) & 1U ? -unit : unit;
                write_pcm(a + (at * sides + side) * bytes, fmt, sample);
            }
            first = Mix_QuickLoad_RAW(a, 4UL * sides * bytes); CHECK(first);
            CHECK(Mix_PlayChannel(0, first, -1) == 0 && Mix_PlayChannel(1, first, -1) == 1);
            render(out, 8);
            for (at = 0; at < 8; ++at) for (side = 0; side < sides; ++side) {
                expected = (at + side) & 1U ? minimum : maximum;
                CHECK(read_pcm(out + (at * sides + side) * bytes, fmt) == expected);
            }
            CHECK(Mix_Volume(0, 64) == 128 && Mix_Volume(1, 64) == 128); render(out, 4);
            for (at = 0; at < 4; ++at) for (side = 0; side < sides; ++side) {
                expected = (at + side) & 1U ? -unit : unit;
                CHECK(read_pcm(out + (at * sides + side) * bytes, fmt) == expected);
            }
            Mix_FreeChunk(first); CHECK(!Mix_Playing(-1)); Mix_CloseAudio();
        }
    }
    printf("SDL_MIXER_DETERMINISTIC passed checks=%u\n", checks);
}
#endif

/* Identical scalar pattern in mixer.py: unequal stereo values, both clipping
 * edges, and three overlapping voices exercise the actual PCM output path. */
static Sint32 voice(unsigned which, unsigned frame, unsigned channel)
{
    static const Sint32 edge[8] = { 24000, -24000, 28000, -28000, 12000, -12000, 30000, -30000 };
    Sint32 base = edge[(frame + channel * 3U) % 8U];
    if (which == 0) return base;
    if (which == 1) return base > 0 ? 16000 : -16000;
    return (frame % 3U) == 0 ? (base > 0 ? -10000 : 10000) : (base > 0 ? 4000 : -4000);
}
static int production(const char *mode)
{
    Uint8 data[3][128]; Mix_Chunk *chunks[3];
    unsigned i, at, channel; Sint32 rate; Uint16 format; int channels;
    CHECK(SDL_Init(SDL_INIT_AUDIO) == 0);
    CHECK(Mix_OpenAudio(8000, AUDIO_S16LSB, 2, 64) == 0);
    CHECK(Mix_QuerySpec(&rate, &format, &channels) > 0 && rate == 8000 && format == AUDIO_S16LSB && channels == 2);
    SDL_PauseAudio(1);
    for (i = 0; i < 3; ++i) {
        for (at = 0; at < 32; ++at) for (channel = 0; channel < 2; ++channel)
            put16(data[i] + at * 4U + channel * 2U, voice(i, at, channel));
        chunks[i] = Mix_QuickLoad_RAW(data[i], 128); CHECK(chunks[i]);
    }
    if (!strcmp(mode, "controls")) {
        CHECK(Mix_AllocateChannels(4) == 4 && Mix_ReserveChannels(2) == 2);
        CHECK(Mix_PlayChannel(-1, chunks[0], -1) == 2 && Mix_PlayChannel(-1, chunks[1], -1) == 3);
        CHECK(Mix_PlayChannel(-1, chunks[2], 0) < 0);
        SDL_LockAudio(); Mix_Pause(-1); CHECK(Mix_Paused(-1) == 2); Mix_Volume(2, 64); SDL_Delay(30);
        CHECK(Mix_Playing(-1) == 2); Mix_Resume(-1); SDL_UnlockAudio();
        SDL_PauseAudio(0); SDL_Delay(100); SDL_PauseAudio(1);
        Mix_FreeChunk(chunks[0]); chunks[0] = NULL; CHECK(!Mix_Playing(2) && Mix_Playing(3));
        Mix_HaltChannel(-1); CHECK(!Mix_Playing(-1)); Mix_CloseAudio();
        CHECK(Mix_OpenAudio(48000L, AUDIO_S16LSB, 2, 64) == 0);
        CHECK(Mix_QuerySpec(&rate, &format, &channels) > 0 && rate == 48000L);
        Mix_CloseAudio();
        printf("SDL_MIXER_CONTROLS passed checks=%u\n", checks);
    } else {
        SDL_LockAudio();
        CHECK(Mix_PlayChannel(0, chunks[0], !strcmp(mode, "loop") ? 2 : -1) == 0);
        if (!strcmp(mode, "overlap")) {
            CHECK(Mix_PlayChannel(1, chunks[1], -1) == 1); CHECK(Mix_PlayChannel(2, chunks[2], -1) == 2);
        } else if (!strcmp(mode, "volume")) {
            CHECK(Mix_Volume(0, 64) == 128 && Mix_VolumeChunk(chunks[0], 64) == 128);
        } else CHECK(!strcmp(mode, "loop") || !strcmp(mode, "pause"));
        SDL_UnlockAudio(); SDL_PauseAudio(0); SDL_Delay(180);
        if (!strcmp(mode, "pause")) { Mix_Pause(0); SDL_Delay(130); Mix_Resume(0); SDL_Delay(160); }
        else SDL_Delay(100);
        SDL_PauseAudio(1); Mix_HaltChannel(-1); Mix_CloseAudio();
        printf("SDL_MIXER_CAPTURE mode=%s checks=%u\n", mode, checks);
    }
    for (i = 0; i < 3; ++i) if (chunks[i]) Mix_FreeChunk(chunks[i]);
    CHECK(SDL_GetAudioStatus() == SDL_AUDIO_STOPPED); SDL_Quit(); return 0;
}

int main(int argc, char **argv)
{
#ifdef MIXER_FAKE_AUDIO
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "--deterministic"))) { deterministic(); return 0; }
#endif
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "--controls"))) return production("controls");
    if (argc == 3 && !strcmp(argv[1], "--capture")) return production(argv[2]);
    fprintf(stderr, "usage: sdl-mixer [--controls | --capture overlap|loop|volume|pause]\n"); return 2;
}
