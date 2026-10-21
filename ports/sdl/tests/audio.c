/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Standard SDL audio calls, exercised by audio.py on a disposable Fuse-X disk.
 * The callback deliberately reads state mutated by the main application. */
#include "SDL.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef FUSE_X_USER
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif

typedef struct AudioProbe {
    unsigned magic, calls, failures, locked, phase, seen;
    unsigned long frames;
    Uint16 format;
    Uint8 channels;
} AudioProbe;

static Sint32 requested_rate = 8000;
static unsigned requested_channels = 2;

static int require(int condition, const char *message)
{
    if (condition) return 1;
    fprintf(stderr, "SDL_AUDIO_FAIL %s: %s\n", message, SDL_GetError());
    return 0;
}

static Sint32 sample_at(unsigned long frame, unsigned channel)
{
    static const Sint32 edges[10] = {
        -32768L, -30001L, -16384L, -257L, -1L,
        1L, 257L, 12345L, 30001L, 32767L
    };
    if (frame < 10UL) return edges[(unsigned)(frame + channel * 7UL) % 10];
    return (Sint32)((frame * 37UL + 19UL + channel * 83UL) % 251UL) * 257L
           - 32123L + (Sint32)channel * 37L;
}

/* Encode logical octets without casting them to a native short. */
static void sample_write(Uint8 *out, Uint16 format, Sint32 sample)
{
    unsigned long value;
    if ((format & 255U) == 8U) {
        value = (unsigned long)(sample + 32768L) / 256UL;
        if (format & 0x8000U) value = (value + 128UL) & 255UL;
        out[0] = (Uint8)value;
        return;
    }
    value = format & 0x8000U ? (unsigned long)sample & 65535UL
                            : (unsigned long)(sample + 32768L);
    if (format & 0x1000U) {
        out[0] = (Uint8)(value >> 8);
        out[1] = (Uint8)(value & 255UL);
    } else {
        out[0] = (Uint8)(value & 255UL);
        out[1] = (Uint8)(value >> 8);
    }
}

static void callback(void *userdata, Uint8 *stream, int length)
{
    AudioProbe *probe = (AudioProbe *)userdata;
    unsigned bytes = (probe->format & 255U) / 8U;
    unsigned stride = bytes * probe->channels, frame, channel, frames;
    if (probe->magic != 0x5a95U || probe->locked || length <= 0 ||
        !stride || (unsigned)length % stride) {
        ++probe->failures;
        return;
    }
    frames = (unsigned)length / stride;
    for (frame = 0; frame < frames; ++frame)
        for (channel = 0; channel < probe->channels; ++channel)
            sample_write(stream + (unsigned long)frame * stride + channel * bytes,
                         probe->format,
                         sample_at(probe->frames + frame + probe->phase * 431UL, channel));
    probe->seen |= 1U << probe->phase;
    ++probe->calls;
    probe->frames += frames;
}

static void request(SDL_AudioSpec *spec, AudioProbe *probe, Uint16 format)
{
    memset(probe, 0, sizeof(*probe));
    probe->magic = 0x5a95U;
    probe->format = format;
    probe->channels = (Uint8)requested_channels;
    memset(spec, 0, sizeof(*spec));
    spec->freq = requested_rate;
    spec->format = format;
    spec->channels = (Uint8)requested_channels;
    spec->samples = 256;
    spec->callback = callback;
    spec->userdata = probe;
}

static int open_audio(SDL_AudioSpec *spec)
{
    SDL_AudioSpec actual;
    if (!require(SDL_OpenAudio(spec, &actual) == 0, "open audio")) return 0;
    return require(actual.freq == spec->freq && actual.format == spec->format &&
                   actual.channels == spec->channels && actual.samples == spec->samples &&
                   actual.size == (Uint32)actual.samples * actual.channels *
                                  ((actual.format & 255U) / 8U),
                   "obtained callback format and length");
}

static void trace(const char *stage, const AudioProbe *probe)
{
    printf("SDL_AUDIO_TRACE %s calls=%u frames=%lu phases=%u\n",
           stage, probe->calls, probe->frames, probe->seen);
    fflush(stdout);
}

static int controls(void)
{
    SDL_AudioSpec spec, invalid;
    AudioProbe probe;
    unsigned previous;
#ifdef FUSE_X_USER
    int fd;
#endif
    if (!require(SDL_Init(SDL_INIT_AUDIO) == 0, "audio-only initialization") ||
        !require(SDL_GetAudioStatus() == SDL_AUDIO_STOPPED, "initial stopped status")) return 0;
    request(&spec, &probe, AUDIO_U16MSB);
    invalid = spec; invalid.callback = NULL;
    if (!require(SDL_OpenAudio(&invalid, NULL) < 0, "reject missing callback")) return 0;
    invalid = spec; invalid.channels = 3;
    if (!require(SDL_OpenAudio(&invalid, NULL) < 0, "reject unsupported channel count")) return 0;
    invalid = spec; invalid.format = 0x1234U;
    if (!require(SDL_OpenAudio(&invalid, NULL) < 0, "reject unsupported format")) return 0;
    invalid = spec; invalid.freq = -1;
    if (!require(SDL_OpenAudio(&invalid, NULL) < 0, "reject invalid rate") ||
        !require(SDL_GetAudioStatus() == SDL_AUDIO_STOPPED, "failed opens retain stopped status")) return 0;
#ifdef FUSE_X_USER
    fd = open("/dev/pcm", O_WRONLY | O_NONBLOCK);
    if (!require(fd >= 0, "claim independent PCM lease")) return 0;
    if (!require(SDL_OpenAudio(&spec, NULL) < 0, "report busy PCM lease")) { close(fd); return 0; }
    close(fd);
#endif
    if (!open_audio(&spec) ||
        !require(SDL_GetAudioStatus() == SDL_AUDIO_PAUSED, "open starts paused")) return 0;
    SDL_Delay(80);
    if (!require(probe.calls == 0, "paused open never invokes callback") ||
        !require(SDL_OpenAudio(&spec, NULL) < 0, "reject duplicate open") ||
        !require(SDL_GetAudioStatus() == SDL_AUDIO_PAUSED, "duplicate open preserves device")) return 0;
#ifdef FUSE_X_USER
    fd = open("/dev/pcm", O_WRONLY | O_NONBLOCK);
    if (!require(fd < 0 && errno == EBUSY, "SDL owns PCM lease")) { if (fd >= 0) close(fd); return 0; }
#endif
    SDL_PauseAudio(0);
    if (!require(SDL_GetAudioStatus() == SDL_AUDIO_PLAYING && probe.calls > 0,
                 "unpause prefills callback before playback")) return 0;
    previous = probe.calls;
    SDL_Delay(180);
    if (!require(probe.calls > previous, "audio-only SDL_Delay keeps callbacks alive")) return 0;
    trace("delay", &probe);
    SDL_LockAudio();
    probe.locked = 1;
    probe.phase = 1;
    previous = probe.calls;
    SDL_Delay(80);
    if (!require(probe.calls == previous, "audio lock suppresses callback reentry")) return 0;
    probe.locked = 0;
    SDL_UnlockAudio();
    SDL_Delay(140);
    if (!require(probe.calls > previous && (probe.seen & 3U) == 3U,
                 "unlock resumes callback with mutated shared userdata")) return 0;
    trace("shared", &probe);
    SDL_PauseAudio(1);
    previous = probe.calls;
    SDL_Delay(80);
    if (!require(SDL_GetAudioStatus() == SDL_AUDIO_PAUSED && probe.calls == previous,
                 "pause freezes callback production")) return 0;
    SDL_PauseAudio(0);
    SDL_Delay(140);
    if (!require(probe.calls > previous && !probe.failures, "resume preserves callback state")) return 0;
    SDL_CloseAudio();
    if (!require(SDL_GetAudioStatus() == SDL_AUDIO_STOPPED, "close stops device")) return 0;
    SDL_CloseAudio();
#ifdef FUSE_X_USER
    fd = open("/dev/pcm", O_WRONLY | O_NONBLOCK);
    if (!require(fd >= 0, "close releases PCM lease")) return 0;
    close(fd);
#endif
    request(&spec, &probe, AUDIO_S16LSB);
    if (!open_audio(&spec)) return 0;
    SDL_PauseAudio(0); SDL_Delay(80);
    if (!require(probe.calls > 0 && !probe.failures, "closed device reopens cleanly")) return 0;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    if (!require(SDL_GetAudioStatus() == SDL_AUDIO_STOPPED, "audio subsystem quit closes device") ||
        !require(SDL_InitSubSystem(SDL_INIT_AUDIO) == 0, "audio subsystem reinitializes")) return 0;
    request(&spec, &probe, AUDIO_S8);
    if (!open_audio(&spec)) return 0;
    SDL_CloseAudio();
    SDL_Quit();
    printf("SDL_AUDIO_CONTROLS passed rate=%ld\n", (long)requested_rate);
    return 1;
}

static Uint16 parse_format(const char *name)
{
    if (!strcmp(name, "u8")) return AUDIO_U8;
    if (!strcmp(name, "s8")) return AUDIO_S8;
    if (!strcmp(name, "u16le")) return AUDIO_U16LSB;
    if (!strcmp(name, "u16be")) return AUDIO_U16MSB;
    if (!strcmp(name, "s16le")) return AUDIO_S16LSB;
    if (!strcmp(name, "s16be")) return AUDIO_S16MSB;
    return 0;
}

static int capture(const char *name)
{
    SDL_AudioSpec spec;
    AudioProbe probe;
    unsigned previous;
    Uint16 format = parse_format(name);
    if (!require(format != 0 || !strcmp(name, "u8"), "known capture format") ||
        !require(SDL_Init(SDL_INIT_AUDIO) == 0, "capture initialization")) return 0;
    request(&spec, &probe, format);
    if (!open_audio(&spec)) return 0;
    SDL_Delay(40);
    if (!require(probe.calls == 0, "capture initially paused")) return 0;
    SDL_PauseAudio(0);
    SDL_Delay(350);
    SDL_PauseAudio(1);
    previous = probe.calls;
    SDL_Delay(80);
    if (!require(probe.calls == previous, "capture pause suppresses callbacks")) return 0;
    SDL_PauseAudio(0);
    SDL_Delay(200);
    SDL_PauseAudio(1);
    SDL_CloseAudio();
    if (!require(probe.calls >= 4 && !probe.failures, "capture callback production")) return 0;
    printf("SDL_AUDIO_CAPTURE format=%s calls=%u generated=%lu rate=%ld channels=%u\n",
           name, probe.calls, probe.frames, (long)requested_rate, requested_channels);
    SDL_Quit();
    return 1;
}

static int wait_event(void)
{
    SDL_AudioSpec spec;
    AudioProbe probe;
    SDL_Event event;
    SDL_Surface *screen;
    unsigned previous;
    Uint32 start, elapsed;
    if (!require(SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO) == 0, "wait initialization")) return 0;
    SDL_WM_SetCaption("SDL audio wait test", NULL);
    screen = SDL_SetVideoMode(320, 240, 32, SDL_SWSURFACE);
    if (!require(screen != NULL, "real GX audio wait window")) return 0;
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 24, 48, 72));
    if (!require(SDL_Flip(screen) == 0, "publish audio wait window")) return 0;
    request(&spec, &probe, AUDIO_S16LSB);
    if (!open_audio(&spec)) return 0;
    SDL_PauseAudio(0);
    SDL_Delay(100);
    while (SDL_PollEvent(&event)) {}
    previous = probe.calls;
    start = SDL_GetTicks();
    printf("SDL_AUDIO_WAIT_READY calls=%u\n", previous);
    fflush(stdout);
    for (;;) {
        if (!require(SDL_WaitEvent(&event) == 1, "wait for scheduled GX key")) return 0;
        if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_SPACE) break;
        if (event.type == SDL_QUIT) return require(0, "window closed before scheduled input");
    }
    elapsed = SDL_GetTicks() - start;
    SDL_PauseAudio(1);
    SDL_CloseAudio();
    if (!require(probe.calls >= previous + 4 && elapsed >= 400U && !probe.failures,
                 "callbacks continue throughout SDL_WaitEvent")) return 0;
    printf("SDL_AUDIO_WAIT passed before=%u after=%u elapsed=%lu generated=%lu\n",
           previous, probe.calls, (unsigned long)elapsed, probe.frames);
    SDL_Quit();
    return 1;
}

int main(int argc, char **argv)
{
    int ok;
    char *end;
    long parameter;
    if ((argc == 2 || argc == 3) && !strcmp(argv[1], "--controls")) {
        if (argc == 3) {
            parameter = strtol(argv[2], &end, 10);
            if (*end || parameter < 8000L || parameter > 96000L) return 2;
            requested_rate = (Sint32)parameter;
        }
        ok = controls();
    }
    else if (argc >= 3 && argc <= 5 && !strcmp(argv[1], "--capture")) {
        if (argc >= 4) {
            parameter = strtol(argv[3], &end, 10);
            if (*end || parameter < 1 || parameter > 2) return 2;
            requested_channels = (unsigned)parameter;
        }
        if (argc == 5) {
            parameter = strtol(argv[4], &end, 10);
            if (*end || parameter < 8000L || parameter > 96000L) return 2;
            requested_rate = (Sint32)parameter;
        }
        ok = capture(argv[2]);
    }
    else if (argc == 2 && !strcmp(argv[1], "--wait")) ok = wait_event();
    else {
        fputs("usage: sdl-audio --controls [RATE] | --capture FORMAT [CHANNELS [RATE]] | --wait\n", stderr);
        return 2;
    }
    if (!ok) SDL_Quit();
    return ok ? 0 : 1;
}
