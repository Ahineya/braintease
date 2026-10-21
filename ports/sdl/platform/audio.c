/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* SDL 1.2 callback audio with explicit cooperative scheduling. The device is
 * asynchronous; callback production takes place only at safe SDL boundaries. */
#include "SDL.h"
#include "audio.h"
#include "audio_formats.h"
#include "platform.h"

static SDL_AudioSpec spec;
static Uint8 *stream;
static Sint16 *samples;
static Uint32 pending, offset, target, queued_at, sampled_at, lock_depth;
static int initialized, opened, paused, servicing, closing, failed;

static void finish_close(void) {
    SDL_PCMClose();
    SDL_free(stream);
    SDL_free(samples);
    stream = NULL;
    samples = NULL;
    SDL_memset(&spec, 0, sizeof(spec));
    pending = offset = target = queued_at = lock_depth = 0;
    opened = closing = failed = 0;
    paused = 1;
}

int SDL_AudioInit(const char *driver) {
#if SDL_RIPLIX_C16
    const char *name = "pcm";
#else
    const char *name = "reference";
#endif
    if (driver && *driver && SDL_strcmp(driver, name)) {
        SDL_SetError("Unsupported SDL audio driver: %s", driver);
        return -1;
    }
    if (servicing) {
        SDL_SetError("Cannot initialize audio inside its callback");
        return -1;
    }
    if (initialized)
        SDL_CloseAudio();
    initialized = 1;
    return 0;
}

void SDL_AudioQuit(void) {
    SDL_CloseAudio();
    initialized = 0;
}

char *SDL_AudioDriverName(char *buffer, int length) {
    if (!initialized || !buffer || length <= 0)
        return NULL;
#if SDL_RIPLIX_C16
    SDL_strlcpy(buffer, "pcm", length);
#else
    SDL_strlcpy(buffer, "reference", length);
#endif
    return buffer;
}

int SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained) {
    SDL_AudioSpec next;
    Uint32 count, capacity, bytes;
    Uint8 *next_stream;
    Sint16 *next_samples;
    if (!desired || !desired->callback) {
        SDL_SetError("Audio needs a specification and callback");
        return -1;
    }
    if (opened || servicing) {
        SDL_SetError("Audio device is already open");
        return -1;
    }
    if (!initialized && SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
        return -1;
    next = *desired;
    if (!next.freq) next.freq = 22050;
    if (!next.format) next.format = AUDIO_S16SYS;
    if (!next.channels) next.channels = 2;
    if (!next.samples) next.samples = 1024;
    if (next.freq < 8000 || next.freq > 96000 ||
        next.channels < 1 || next.channels > 2 ||
        !SDL_C16AudioFormatValid(next.format) ||
        (next.samples & (next.samples - 1))) {
        SDL_SetError("Audio requires 8-96 kHz, mono/stereo, PCM8/16 and power-of-two samples");
        return -1;
    }
    bytes = (next.format & 255U) / 8U;
    count = (Uint32)next.samples * next.channels;
    /* Callback len remains int in SDL 1.2, including on C16. Do not truncate
     * lengths or callbacks, and stay within the device's two-second bound. */
    if (count * bytes > 32767UL || (Uint32)next.samples > (Uint32)next.freq * 2UL) {
        SDL_SetError("Audio callback buffer exceeds the callback or PCM queue bounds");
        return -1;
    }
    next.size = count * bytes;
    next.silence = next.format == AUDIO_U8 ? 128 : 0;
    next.padding = 0;
    capacity = (Uint32)next.samples * 4UL;
    if (capacity > (Uint32)next.freq * 2UL)
        capacity = (Uint32)next.freq * 2UL;
    next_stream = SDL_malloc(next.size * sizeof(*next_stream));
    next_samples = SDL_malloc(count * sizeof(*next_samples));
    if (!next_stream || !next_samples) {
        SDL_free(next_stream);
        SDL_free(next_samples);
        SDL_OutOfMemory();
        return -1;
    }
    if (SDL_PCMOpen(next.freq, next.channels, capacity) < 0) {
        SDL_free(next_stream);
        SDL_free(next_samples);
        return -1;
    }
    spec = next;
    stream = next_stream;
    samples = next_samples;
    target = capacity > (Uint32)next.samples ? capacity - next.samples : capacity;
    if (target < next.samples) target = next.samples;
    pending = offset = queued_at = lock_depth = 0;
    paused = opened = 1;
    failed = closing = 0;
    *desired = next;
    if (obtained) *obtained = next;
    return 0;
}

SDL_audiostatus SDL_GetAudioStatus(void) {
    if (!opened || failed || closing) return SDL_AUDIO_STOPPED;
    return paused ? SDL_AUDIO_PAUSED : SDL_AUDIO_PLAYING;
}

Uint16 SDL_C16AudioMixFormat(void) {
    return opened && !closing ? spec.format : 0;
}

static void fail_device(void) {
    failed = paused = 1;
    /* Close releases the exclusive lease and discards queued speaker output.
     * Defer freeing callback storage until the active service frame returns. */
    if (servicing) closing = 1;
    else finish_close();
}

static void silence_stream(void) {
    Uint32 i;
    SDL_memset(stream, spec.silence, spec.size * sizeof(*stream));
    /* SDL's single silence octet cannot describe unsigned 16-bit midpoint. */
    if (spec.format == AUDIO_U16LSB || spec.format == AUDIO_U16MSB) {
        unsigned high = spec.format == AUDIO_U16MSB ? 0 : 1;
        for (i = high; i < spec.size; i += 2)
            stream[i] = 128;
    }
}

void SDL_C16AudioPump(void) {
    Uint32 queued, capacity, frames, i, count;
    unsigned callbacks = 0;
    Sint32 accepted, value;
    unsigned bytes;
    if (!opened || paused || failed || servicing || lock_depth || closing)
        return;
    servicing = 1;
    if (SDL_PCMState(&queued, &capacity) < 0) {
        fail_device();
        goto done;
    }
    if (!capacity || queued > capacity) {
        SDL_SetError("PCM returned invalid queue bounds");
        fail_device();
        goto done;
    }
    while (opened && !paused && !failed && !closing && !lock_depth) {
        if (pending) {
            frames = pending - offset;
            if (frames > capacity - queued) frames = capacity - queued;
            if (!frames) break;
            accepted = SDL_PCMWrite(samples + offset * spec.channels, frames);
            if (accepted < 0) {
                fail_device();
                break;
            }
            if (!accepted) break;
            if ((Uint32)accepted > frames) {
                SDL_SetError("PCM accepted more frames than submitted");
                fail_device();
                break;
            }
            offset += accepted;
            queued += accepted;
            if (offset != pending) continue;
            pending = offset = 0;
        }
        if (queued >= target || callbacks == 4)
            break;
        silence_stream();
        spec.callback(spec.userdata, stream, (int)spec.size);
        callbacks++;
        if (closing || paused || failed)
            break;
        count = (Uint32)spec.samples * spec.channels;
        bytes = (spec.format & 255U) / 8U;
        for (i = 0; i < count; i++) {
            value = SDL_C16AudioReadSample(stream + i * bytes, spec.format);
            if (bytes == 1) {
                if (!(spec.format & 0x8000U)) value -= 128;
                value *= 256;
            } else if (!(spec.format & 0x8000U)) value -= 32768L;
            samples[i] = (Sint16)value;
        }
        pending = spec.samples;
    }
    if (!closing && !failed) {
        queued_at = queued;
        sampled_at = SDL_GX_GetTicks();
    }
done:
    servicing = 0;
    if (closing) finish_close();
}

Uint32 SDL_C16AudioWait(Uint32 maximum) {
    Uint32 elapsed, frames, low, delay;
    if (!opened || paused || failed || closing || servicing || lock_depth)
        return maximum;
    elapsed = SDL_GX_GetTicks() - sampled_at;
    /* Only a bounded queue can matter; avoid overflowing a long after idle. */
    if (elapsed > 2000UL) return maximum ? 1 : 0;
    frames = elapsed * (Uint32)spec.freq / 1000UL;
    low = target > spec.samples ? target - spec.samples : 0;
    if (pending || frames >= queued_at || queued_at - frames <= low)
        delay = 1;
    else
        delay = ((queued_at - frames - low) * 1000UL + spec.freq - 1UL) / spec.freq;
    return delay < maximum ? delay : maximum;
}

void SDL_PauseAudio(int pause_on) {
    if (!opened || failed || closing) return;
    if (pause_on) {
        paused = 1;
        if (SDL_PCMPause() < 0) fail_device();
    } else if (paused) {
        paused = 0;
        /* Fill the stopped queue before starting its clock/speaker. */
        SDL_C16AudioPump();
        if (opened && !failed && !closing && !paused && SDL_PCMStart() < 0)
            fail_device();
    }
}

void SDL_LockAudio(void) {
    if (opened && lock_depth != 0xffffffffUL) lock_depth++;
}

void SDL_UnlockAudio(void) {
    if (lock_depth) {
        lock_depth--;
        if (!lock_depth) SDL_C16AudioPump();
    }
}

void SDL_CloseAudio(void) {
    if (!opened) return;
    if (servicing) {
        closing = paused = 1;
        return;
    }
    finish_close();
}
