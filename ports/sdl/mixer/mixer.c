/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Original implementation of a bounded SDL_mixer 1.2 PCM effects API subset.
 * No upstream SDL_mixer implementation is incorporated in this file.
 */
#include "SDL_mixer.h"
#include "audio_formats.h"

typedef struct Mix_Voice {
    Mix_Chunk *chunk;
    Uint32 position, remaining, fade_position, fade_frames;
    int playing, paused, loops, volume, expires, fade_start;
    Mix_Fading fading;
} Mix_Voice;

static SDL_AudioSpec device;
static Mix_Voice *voices;
static int voice_count, reserved, opened, dispatching;
static Uint8 *finished;
static int finished_capacity, pending_finished;
static void (SDLCALL *finished_hook)(int);

Sint32 SDLCALL Mix_PCMHelperPID(void) { return 0; }

static int bounded_volume(int value)
{
    return value > MIX_MAX_VOLUME ? MIX_MAX_VOLUME : value;
}

static int valid_channel(int channel)
{
    return channel >= 0 && channel < voice_count;
}

/* floor(part * scale / whole), with no wide scalar or overflowing product.
 * All callers provide part <= whole and scale <= 96000. */
static Uint32 fraction(Uint32 part, Uint32 whole, Uint32 scale)
{
    Uint32 bit = 65536UL, result = 0, remainder = 0;
    if (!whole || part >= whole) return scale;
    while (bit > scale && bit > 1) bit >>= 1;
    for (; bit; bit >>= 1) {
        result *= 2;
        if (remainder >= whole - remainder) {
            remainder -= whole - remainder;
            result++;
        } else remainder *= 2;
        if (scale & bit) {
            if (remainder >= whole - part) {
                remainder -= whole - part;
                result++;
            } else remainder += part;
        }
    }
    return result;
}

static Uint32 duration_frames(Sint32 ms)
{
    Uint32 seconds, tail, rate = (Uint32)device.freq;
    if (ms <= 0 || !rate) return 0;
    seconds = (Uint32)ms / 1000UL;
    tail = (((Uint32)ms % 1000UL) * rate + 999UL) / 1000UL;
    if (seconds > (0xffffffffUL - tail) / rate) return 0xffffffffUL;
    return seconds * rate + tail;
}

static void finish_voice(int channel)
{
    Mix_Voice *voice = &voices[channel];
    if (voice->playing && !finished[channel]) {
        finished[channel] = 1;
        pending_finished++;
    }
    voice->playing = voice->paused = voice->expires = 0;
    voice->position = voice->remaining = 0;
    voice->fading = MIX_NO_FADING;
}

/* Dispatch only after an operation or the complete render block has released
 * every voice/chunk pointer. A hook may free chunks, resize voices or close.
 * Recursive hooks are flattened, with a finite dispatch budget per boundary. */
static void dispatch_finished(void)
{
    int i = 0, channel, budget = finished_capacity;
    if (dispatching) return;
    dispatching = 1;
    while (pending_finished && budget--) {
        while (!finished[i]) {
            i++;
            if (i == finished_capacity) i = 0;
        }
        channel = i;
        finished[i] = 0; pending_finished--;
        i++;
        if (i == finished_capacity) i = 0;
        if (finished_hook) finished_hook(channel);
    }
    dispatching = 0;
    if (!opened && !voice_count && !pending_finished) {
        SDL_free(finished); finished = NULL; finished_capacity = 0;
    }
}

static int fade_factor(const Mix_Voice *voice)
{
    if (voice->fading == MIX_FADING_IN)
        return (int)fraction(voice->fade_position, voice->fade_frames, 128);
    if (voice->fading == MIX_FADING_OUT)
        return voice->fade_start -
            (int)fraction(voice->fade_position, voice->fade_frames,
                          (Uint32)voice->fade_start);
    return 128;
}

static void SDLCALL mix_callback(void *unused, Uint8 *stream, int len)
{
    Uint32 frame, frame_count, byte, at;
    Sint32 sum[2], sample, minimum, maximum, bias;
    int channel, side, factor, volume, chunk_volume, output_channels;
    Uint32 bytes, frame_bytes;
    Mix_Voice *voice;
    (void)unused;
    if (len <= 0 || !stream) return;
    output_channels = device.channels;
    if (output_channels < 1 || output_channels > 2) return;
    bytes = (device.format & 255U) / 8;
    frame_bytes = bytes * (Uint32)output_channels;
    minimum = bytes == 1 ? -128 : -32768L;
    maximum = bytes == 1 ? 127 : 32767L;
    bias = (device.format & 0x8000U) ? 0 : -minimum;
    /* A normal backend always supplies full frames. Initialize even a short
     * trailing octet, then overwrite complete samples with encoded silence. */
    for (byte = 0; byte < (Uint32)len; byte++) stream[byte] = 0;
    for (byte = 0; byte + bytes <= (Uint32)len; byte += bytes)
        SDL_C16AudioWriteSample(stream + byte, device.format, bias);
    frame_count = (Uint32)len / frame_bytes;
    for (frame = 0; frame < frame_count; ++frame) {
        sum[0] = sum[1] = 0;
        for (channel = 0; channel < voice_count; ++channel) {
            voice = &voices[channel];
            if (!voice->playing || voice->paused) continue;
            if ((voice->expires && !voice->remaining) ||
                (voice->fading == MIX_FADING_OUT &&
                 voice->fade_position == voice->fade_frames)) {
                finish_voice(channel);
                continue;
            }
            if (voice->fading == MIX_FADING_IN &&
                voice->fade_position == voice->fade_frames)
                voice->fading = MIX_NO_FADING;
            factor = fade_factor(voice);
            /* Mix_Chunk is public; clamp even a directly modified carrier so
             * its value cannot overflow a C16 int or the sum's proof. */
            chunk_volume = voice->chunk->volume > 128 ? 128 : (int)voice->chunk->volume;
            volume = (voice->volume * chunk_volume) / 128;
            volume = (volume * factor) / 128;
            for (side = 0; side < output_channels; ++side) {
                at = voice->position + (Uint32)side * bytes;
                sample = SDL_C16AudioReadSample(voice->chunk->abuf + at, device.format) - bias;
                sum[side] += sample * (Sint32)volume / 128;
            }
            voice->position += frame_bytes;
            if (voice->expires) voice->remaining--;
            if (voice->fading != MIX_NO_FADING &&
                voice->fade_position < voice->fade_frames)
                voice->fade_position++;
            if (voice->position == voice->chunk->alen) {
                if (voice->loops) {
                    if (voice->loops > 0) voice->loops--;
                    voice->position = 0;
                } else finish_voice(channel);
            }
        }
        for (side = 0; side < output_channels; ++side) {
            sample = sum[side];
            if (sample < minimum) sample = minimum;
            if (sample > maximum) sample = maximum;
            SDL_C16AudioWriteSample(stream + frame * frame_bytes + (Uint32)side * bytes,
                                    device.format, sample + bias);
        }
    }
    /* An exact block boundary is observable to Playing/Fading/finished hooks. */
    for (channel = 0; channel < voice_count; ++channel) {
        voice = &voices[channel];
        if (voice->playing && !voice->paused) {
            if ((voice->expires && !voice->remaining) ||
                (voice->fading == MIX_FADING_OUT &&
                 voice->fade_position == voice->fade_frames)) finish_voice(channel);
            else if (voice->fading == MIX_FADING_IN &&
                     voice->fade_position == voice->fade_frames)
                voice->fading = MIX_NO_FADING;
        }
    }
    dispatch_finished();
}

int SDLCALL Mix_AllocateChannels(int count)
{
    Mix_Voice *next;
    Uint8 *next_finished = NULL;
    int i, previous;
    if (count < 0) return voice_count;
    if (count > MIX_PCM_MAX_CHANNELS ||
        (Uint32)count > 0x7fffffffUL / sizeof(*next) ||
        (Uint32)count > 0x7fffffffUL / sizeof(*next_finished)) {
        SDL_SetError("PCM mixer supports at most %d channels", MIX_PCM_MAX_CHANNELS);
        return -1;
    }
    SDL_LockAudio();
    if (count == voice_count) { SDL_UnlockAudio(); return count; }
    next = count ? (Mix_Voice *)SDL_calloc((Uint32)count, sizeof(*next)) : NULL;
    if (count && !next) {
        SDL_OutOfMemory(); SDL_UnlockAudio(); return -1;
    }
    if (count > finished_capacity) {
        next_finished = (Uint8 *)SDL_calloc((Uint32)count, sizeof(*next_finished));
        if (!next_finished) {
            SDL_free(next); SDL_OutOfMemory(); SDL_UnlockAudio(); return -1;
        }
        if (finished_capacity)
            SDL_memcpy(next_finished, finished, (Uint32)finished_capacity * sizeof(*finished));
    }
    previous = voice_count < count ? voice_count : count;
    for (i = 0; i < previous; ++i) next[i] = voices[i];
    for (i = previous; i < count; ++i) next[i].volume = 128;
    for (i = count; i < voice_count; ++i) finish_voice(i);
    SDL_free(voices);
    voices = next; voice_count = count;
    if (next_finished) {
        SDL_free(finished); finished = next_finished; finished_capacity = count;
    }
    if (reserved > count) reserved = count;
    dispatch_finished();
    count = voice_count;
    SDL_UnlockAudio();
    return count;
}

int SDLCALL Mix_OpenAudio(Sint32 frequency, Uint16 format, int channels, int chunksize)
{
    SDL_AudioSpec desired;
    if (opened) {
        if (frequency != device.freq || format != device.format || channels != device.channels) {
            SDL_SetError("PCM mixer is already open with a different specification"); return -1;
        }
        if (opened == 32767) { SDL_SetError("Too many PCM mixer opens"); return -1; }
        opened++; return 0;
    }
    if (frequency < 8000 || frequency > 96000 ||
        !SDL_C16AudioFormatValid(format) || (channels != 1 && channels != 2) ||
        chunksize <= 0 || chunksize > 16384 || (chunksize & (chunksize - 1)) ||
        (Uint32)chunksize * (Uint32)channels * ((format & 255U) / 8) > 32767UL) {
        SDL_SetError("PCM mixer requires 8-96 kHz PCM8/16, mono/stereo and power-of-two buffers");
        return -1;
    }
    SDL_memset(&desired, 0, sizeof(desired));
    desired.freq = frequency; desired.format = format; desired.channels = (Uint8)channels;
    desired.samples = (Uint16)chunksize; desired.callback = mix_callback;
    if (SDL_OpenAudio(&desired, &device) < 0) return -1;
    if (device.freq < 8000 || device.freq > 96000 ||
        !SDL_C16AudioFormatValid(device.format) ||
        (device.channels != 1 && device.channels != 2) || !device.samples ||
        (Uint32)device.samples * device.channels * ((device.format & 255U) / 8) > 32767UL) {
        SDL_CloseAudio();
        SDL_SetError("Obtained audio specification exceeds PCM mixer bounds");
        return -1;
    }
    if (Mix_AllocateChannels(MIX_CHANNELS) < 0) { SDL_CloseAudio(); return -1; }
    opened = 1;
    SDL_PauseAudio(0);
    return 0;
}

void SDLCALL Mix_CloseAudio(void)
{
    int i;
    if (!opened) return;
    if (--opened) return;
    SDL_LockAudio();
    for (i = 0; i < voice_count; ++i) finish_voice(i);
    SDL_CloseAudio();
    SDL_free(voices); voices = NULL; voice_count = reserved = 0;
    dispatch_finished();
    SDL_UnlockAudio();
}

int SDLCALL Mix_QuerySpec(Sint32 *frequency, Uint16 *format, int *channels)
{
    if (!opened) return 0;
    if (frequency) *frequency = device.freq;
    if (format) *format = device.format;
    if (channels) *channels = device.channels;
    return opened;
}

int SDLCALL Mix_ReserveChannels(int count)
{
    SDL_LockAudio();
    if (count >= 0) reserved = count > voice_count ? voice_count : count;
    count = reserved;
    SDL_UnlockAudio();
    return count;
}

#include "chunk.inc"

Mix_Chunk * SDLCALL Mix_LoadWAV_RW(SDL_RWops *src, int freesrc)
{
    if (!opened) {
        SDL_SetError("PCM mixer audio is not open");
        if (src && freesrc) SDL_RWclose(src);
        return NULL;
    }
    return mix_load_wav(src, freesrc, device);
}

Mix_Chunk * SDLCALL Mix_QuickLoad_RAW(Uint8 *mem, Uint32 len)
{
    Mix_Chunk *chunk;
    Uint32 frame_bytes;
    if (!opened) { SDL_SetError("PCM mixer audio is not open"); return NULL; }
    frame_bytes = (device.format & 255U) / 8 * device.channels;
    if (!mem || !len || len % frame_bytes || len > 0x7fffffffUL / sizeof(Uint8)) {
        SDL_SetError("RAW PCM requires nonempty frame-aligned storage"); return NULL;
    }
    chunk = (Mix_Chunk *)SDL_malloc(sizeof(*chunk));
    if (!chunk) { SDL_OutOfMemory(); return NULL; }
    chunk->allocated = 0; chunk->abuf = mem; chunk->alen = len; chunk->volume = 128;
    return chunk;
}

void SDLCALL Mix_FreeChunk(Mix_Chunk *chunk)
{
    int i;
    if (!chunk) return;
    SDL_LockAudio();
    for (i = 0; i < voice_count; ++i) {
        if (voices[i].chunk == chunk) { finish_voice(i); voices[i].chunk = NULL; }
    }
    if (chunk->allocated) SDL_free(chunk->abuf);
    SDL_free(chunk);
    dispatch_finished();
    SDL_UnlockAudio();
}

static int play(int channel, Mix_Chunk *chunk, int loops, Sint32 ms, Sint32 ticks)
{
    Mix_Voice *voice;
    Uint32 frame_bytes;
    int i;
    if (!opened) { SDL_SetError("PCM mixer audio is not open"); return -1; }
    frame_bytes = (device.format & 255U) / 8 * device.channels;
    if (!chunk || !chunk->abuf || !chunk->alen || chunk->alen % frame_bytes || loops < -1) {
        SDL_SetError("Invalid PCM chunk or loop count"); return -1;
    }
    SDL_LockAudio();
    if (channel == -1) {
        for (i = reserved; i < voice_count && voices[i].playing; ++i) {}
        channel = i;
    }
    if (!valid_channel(channel)) {
        SDL_SetError("No available PCM mixer channel"); SDL_UnlockAudio(); return -1;
    }
    voice = &voices[channel];
    finish_voice(channel);
    voice->chunk = chunk; voice->position = 0;
    voice->playing = 1; voice->paused = 0; voice->loops = loops;
    voice->expires = ticks >= 0; voice->remaining = duration_frames(ticks);
    voice->fading = ms > 0 ? MIX_FADING_IN : MIX_NO_FADING;
    voice->fade_position = 0; voice->fade_frames = duration_frames(ms);
    voice->fade_start = 128;
    if (ticks == 0) finish_voice(channel);
    dispatch_finished();
    SDL_UnlockAudio();
    return channel;
}

int SDLCALL Mix_PlayChannelTimed(int channel, Mix_Chunk *chunk, int loops, Sint32 ticks)
{
    return play(channel, chunk, loops, 0, ticks);
}

int SDLCALL Mix_FadeInChannelTimed(int channel, Mix_Chunk *chunk, int loops, Sint32 ms, Sint32 ticks)
{
    return play(channel, chunk, loops, ms, ticks);
}

int SDLCALL Mix_HaltChannel(int channel)
{
    int i;
    SDL_LockAudio();
    for (i = 0; i < voice_count; ++i)
        if (channel == -1 || channel == i) finish_voice(i);
    dispatch_finished();
    SDL_UnlockAudio();
    return 0;
}

int SDLCALL Mix_ExpireChannel(int channel, Sint32 ticks)
{
    int i, count = 0;
    SDL_LockAudio();
    for (i = 0; i < voice_count; ++i) {
        if (channel != -1 && channel != i) continue;
        voices[i].expires = ticks >= 0;
        voices[i].remaining = duration_frames(ticks);
        if (ticks == 0) finish_voice(i);
        count++;
    }
    dispatch_finished();
    SDL_UnlockAudio();
    return count;
}

int SDLCALL Mix_Volume(int channel, int volume)
{
    Sint32 total = 0;
    int i, count = 0;
    volume = bounded_volume(volume);
    SDL_LockAudio();
    for (i = 0; i < voice_count; ++i) {
        if (channel != -1 && channel != i) continue;
        total += voices[i].volume; count++;
        if (volume >= 0) voices[i].volume = volume;
    }
    SDL_UnlockAudio();
    return count ? (int)(total / count) : -1;
}

int SDLCALL Mix_VolumeChunk(Mix_Chunk *chunk, int volume)
{
    int previous;
    if (!chunk) return -1;
    SDL_LockAudio();
    previous = chunk->volume;
    if (volume >= 0) chunk->volume = (Uint8)bounded_volume(volume);
    SDL_UnlockAudio();
    return previous;
}

static void pause_channels(int channel, int pause)
{
    int i;
    SDL_LockAudio();
    for (i = 0; i < voice_count; ++i)
        if ((channel == -1 || channel == i) && voices[i].playing) voices[i].paused = pause;
    SDL_UnlockAudio();
}

void SDLCALL Mix_Pause(int channel) { pause_channels(channel, 1); }
void SDLCALL Mix_Resume(int channel) { pause_channels(channel, 0); }

static int count_channels(int channel, int paused)
{
    int i, count = 0;
    SDL_LockAudio();
    for (i = 0; i < voice_count; ++i)
        if ((channel == -1 || channel == i) && voices[i].playing &&
            (!paused || voices[i].paused)) count++;
    SDL_UnlockAudio();
    return count;
}

int SDLCALL Mix_Paused(int channel) { return count_channels(channel, 1); }
int SDLCALL Mix_Playing(int channel) { return count_channels(channel, 0); }

int SDLCALL Mix_FadeOutChannel(int channel, Sint32 ms)
{
    int i, count = 0;
    SDL_LockAudio();
    for (i = 0; i < voice_count; ++i) {
        if ((channel != -1 && channel != i) || !voices[i].playing) continue;
        voices[i].fade_start = fade_factor(&voices[i]);
        voices[i].fading = MIX_FADING_OUT;
        voices[i].fade_position = 0;
        voices[i].fade_frames = duration_frames(ms);
        if (ms <= 0) finish_voice(i);
        count++;
    }
    dispatch_finished();
    SDL_UnlockAudio();
    return count;
}

Mix_Fading SDLCALL Mix_FadingChannel(int channel)
{
    Mix_Fading fading;
    SDL_LockAudio();
    fading = valid_channel(channel) ? voices[channel].fading : MIX_NO_FADING;
    SDL_UnlockAudio();
    return fading;
}

void SDLCALL Mix_ChannelFinished(void (SDLCALL *callback)(int))
{
    SDL_LockAudio(); finished_hook = callback; SDL_UnlockAudio();
}

Mix_Chunk * SDLCALL Mix_GetChunk(int channel)
{
    Mix_Chunk *chunk;
    SDL_LockAudio();
    chunk = valid_channel(channel) ? voices[channel].chunk : NULL;
    SDL_UnlockAudio();
    return chunk;
}

static int unsupported_music(void)
{
    SDL_SetError("This PCM/WAV effects profile does not support music or compressed codecs");
    return -1;
}

Mix_Music * SDLCALL Mix_LoadMUS(const char *file)
{
    (void)file; unsupported_music(); return NULL;
}
Mix_Music * SDLCALL Mix_LoadMUS_RW(SDL_RWops *src)
{
    (void)src; unsupported_music(); return NULL;
}
int SDLCALL Mix_PlayMusic(Mix_Music *music, int loops)
{
    (void)music; (void)loops; return unsupported_music();
}
int SDLCALL Mix_FadeInMusic(Mix_Music *music, int loops, Sint32 ms)
{
    (void)music; (void)loops; (void)ms; return unsupported_music();
}
int SDLCALL Mix_HaltMusic(void) { return unsupported_music(); }
int SDLCALL Mix_PlayingMusic(void) { return 0; }
void SDLCALL Mix_FreeMusic(Mix_Music *music) { (void)music; }
