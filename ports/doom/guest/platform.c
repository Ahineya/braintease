/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <gx.h>
#include "guest.h"
#include "doom_port.h"
#include "doom_resolution.h"
#include "i_system.h"
#if DOOM_PCM
#include "audio.h"
#include <sys/pcm.h>
#endif

static int graphics = -1;
static struct gx_window window;
static struct gx_info info;
static uint16_t *scaled;
static unsigned *columns;
static unsigned view_width, view_height, view_x, view_y;
static unsigned long epoch;
static int clock_started;
static unsigned long first_frame_ms;
#if DOOM_PCM
static struct pcm_state first_audio;
#endif

/* The engine's mouse-speed widget references this even in keyboard mode. */
float fabsf(float value) { return value < 0.0f ? -value : value; }

static unsigned long milliseconds(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) I_Error("Cannot read monotonic clock");
    return (unsigned long)t.tv_sec * 1000UL + (unsigned long)t.tv_nsec / 1000000UL;
}

void Doom_GuestTick(uint32_t tick)
{
    unsigned long now, target;
    if (doom_guest_fast) return;
    now = milliseconds();
    if (!clock_started) { epoch = now; clock_started = 1; }
    target = (tick / 35UL) * 1000UL + (tick % 35UL) * 1000UL / 35UL;
    while ((unsigned long)(now - epoch) < target) {
        usleep((target - (now - epoch)) * 1000UL);
        now = milliseconds();
    }
    /* A paused window must not cause minutes of unpaced catch-up. */
    if ((unsigned long)(now - epoch) > target + 250UL) epoch = now - target;
}

void Doom_GuestShutdown(void)
{
    if (graphics >= 0) {
        struct gx_handle handle;
        handle.version = GX_VERSION;
        handle.handle = window.window;
        if (window.window) ioctl(graphics, GX_DESTROY, &handle);
        close(graphics);
        graphics = -1;
        window.window = 0;
    }
    free(scaled); scaled = NULL;
    free(columns); columns = NULL;
}

static void resize_picture(unsigned width, unsigned height)
{
    struct gx_configure config;
    uint16_t *replacement;
    unsigned *map;
    unsigned x;
    if (!width || !height || width > info.max_width || height > info.max_height) return;
    replacement = NULL; map = NULL;
    if (!(info.flags & GX_CAP_SCALED_UPLOAD)) {
        replacement = calloc((unsigned long)width * height, sizeof(*replacement));
        map = malloc((unsigned long)width * sizeof(*map));
        if (!replacement || !map) { free(replacement); free(map); return; }
    }
    config.version = GX_VERSION; config.window = window.window;
    config.width = width; config.height = height; config.flags = 0;
    if (ioctl(graphics, GX_CONFIGURE, &config)) { free(replacement); free(map); return; }
    free(scaled); free(columns);
    scaled = replacement; columns = map;
    window.width = width; window.height = height;
    if (info.flags & GX_CAP_SCALED_UPLOAD) return;
    view_width = width; view_height = (unsigned long)width * DOOM_OUTPUT_HEIGHT / DOOM_OUTPUT_WIDTH;
    if (view_height > height) {
        view_height = height; view_width = (unsigned long)height * DOOM_OUTPUT_WIDTH / DOOM_OUTPUT_HEIGHT;
    }
    if (!view_width) view_width = 1;
    if (!view_height) view_height = 1;
    view_x = (width - view_width) / 2; view_y = (height - view_height) / 2;
    for (x = 0; x < view_width; ++x) columns[x] = (unsigned long)x * DOOM_OUTPUT_WIDTH / view_width;
}

void Doom_GuestGraphics(void)
{
    graphics = open("/dev/gfx", O_RDWR);
    if (graphics < 0) I_Error("Cannot open /dev/gfx");
    memset(&info, 0, sizeof(info)); info.version = GX_VERSION;
    if (ioctl(graphics, GX_INFO, &info)) I_Error("Cannot query graphics capabilities");
    atexit(Doom_GuestShutdown);
    memset(&window, 0, sizeof(window));
    window.version = GX_VERSION;
    window.width = DOOM_OUTPUT_WIDTH;
    window.height = DOOM_OUTPUT_HEIGHT;
    window.format = GX_RGB565;
    strcpy(window.title, "Doom");
    if (ioctl(graphics, GX_CREATE, &window)) I_Error("Cannot create Doom window");
    if (info.flags & GX_CAP_RESIZE) resize_picture(DOOM_OUTPUT_WIDTH * 3U, DOOM_OUTPUT_HEIGHT * 3U);
}

void Doom_GuestInput(void)
{
    struct gx_event event;
    struct gx_state state;
    unsigned int i;
    if (graphics < 0) return;
    for (;;) {
        memset(&event, 0, sizeof(event));
        event.version = GX_VERSION;
        event.window = window.window;
        if (ioctl(graphics, GX_EVENT, &event)) I_Error("Cannot read Doom input");
        if (!event.type) return;
        if (event.type == GX_CLOSE) exit(0);
        if (event.type == GX_RESIZE && event.x > 0 && event.y > 0
            && (unsigned long)event.x <= info.max_width && (unsigned long)event.y <= info.max_height)
            resize_picture((unsigned)event.x, (unsigned)event.y);
        if (event.type == GX_KEY) Doom_PostKey(event.code, (event.flags & GX_DOWN) != 0);
        if (event.type == GX_FOCUS && !(event.flags & GX_FOCUSED)) {
            for (i = 0; i < 256; ++i) Doom_PostKey(i, 0);
        }
        if (event.type == GX_OVERFLOW) {
            memset(&state, 0, sizeof(state));
            state.version = GX_VERSION;
            state.window = window.window;
            if (ioctl(graphics, GX_STATE, &state)) I_Error("Cannot recover Doom input");
            for (i = 0; i < 256; ++i)
                Doom_PostKey(i, (state.flags & GX_FOCUSED) && (state.held[i / 16] & (1U << (i % 16))));
        }
    }
}

int Doom_GuestIndexed(void) { return (info.flags & GX_CAP_INDEXED_UPLOAD) != 0; }
static void present(const uint16_t *pixels, const uint16_t *palette, uint32_t frame)
{
    struct gx_upload upload;
    struct gx_handle handle;
    if (frame == 1 && clock_started) {
        uint32_t tick = Doom_Tick();
        /* Initial level loading is not lost simulation time. Rebase before
         * real-time playback instead of racing to catch up startup ticks. */
        epoch = milliseconds() - ((tick / 35UL) * 1000UL + (tick % 35UL) * 1000UL / 35UL);
    }
#if DOOM_PCM
    /* Delay physical playback through startup and initial level loading. */
    Doom_GuestAudioPump();
#endif
    if (doom_guest_stats && frame == 1) {
        first_frame_ms = milliseconds();
#if DOOM_PCM
        memset(&first_audio, 0, sizeof(first_audio));
        if (Doom_GuestAudioFd() >= 0) ioctl(Doom_GuestAudioFd(), PCM_STATE, &first_audio);
#endif
    }
    if (scaled) {
        unsigned x, y;
        for (y = 0; y < view_height; ++y) {
            const uint16_t *source = pixels + ((unsigned long)y * DOOM_OUTPUT_HEIGHT / view_height) * DOOM_OUTPUT_WIDTH;
            uint16_t *dest = scaled + (unsigned long)(view_y + y) * window.width + view_x;
            for (x = 0; x < view_width; ++x) dest[x] = source[columns[x]];
        }
        pixels = scaled;
    }
    memset(&upload, 0, sizeof(upload));
    upload.version = GX_VERSION;
    upload.surface = window.surface;
    upload.width = (info.flags & GX_CAP_SCALED_UPLOAD) ? DOOM_OUTPUT_WIDTH : window.width;
    upload.height = (info.flags & GX_CAP_SCALED_UPLOAD) ? DOOM_OUTPUT_HEIGHT : window.height;
    upload.stride = upload.width;
    upload.pixels = pixels;
    handle.version = GX_VERSION;
    handle.handle = window.window;
    if (palette) {
        struct gx_indexed_upload indexed;
        indexed.image = upload; indexed.palette = palette;
        if (ioctl(graphics, GX_UPLOAD_INDEXED, &indexed)) I_Error("Cannot upload indexed Doom frame");
    } else if (ioctl(graphics, (info.flags & GX_CAP_SCALED_UPLOAD) ? GX_UPLOAD_SCALED : GX_UPLOAD, &upload))
        I_Error("Cannot upload Doom frame");
    if (ioctl(graphics, GX_PRESENT, &handle))
        I_Error("Cannot present Doom frame");
    if (doom_guest_frames && frame >= doom_guest_frames) {
        if (doom_guest_stats) {
            printf("DOOM_STATS frames=%lu elapsed_ms=%lu width=%u height=%u\n", (unsigned long)frame - 1UL, milliseconds() - first_frame_ms, window.width, window.height);
#if DOOM_PCM
            if (Doom_GuestAudioFd() >= 0) {
                struct pcm_state state;
                struct pcm_output_stats output;
                if (!ioctl(Doom_GuestAudioFd(), PCM_STATE, &state))
                    printf("DOOM_AUDIO frames=%lu underruns=%lu queued=%lu late_ms=%lu dropped=%lu\n", state.position - first_audio.position,
                        state.underruns - first_audio.underruns, state.queued, state.late_ms - first_audio.late_ms, Doom_GuestAudioDropped());
                if (!ioctl(Doom_GuestAudioFd(), PCM_OUTPUT_STATS, &output))
                    printf("DOOM_SPEAKER live=%u gaps=%lu max_gap_ms=%lu\n", output.live, output.gaps, output.max_gap_ms);
            }
#endif
        }
        printf("DOOM_GUEST_OK frames=%lu tick=%lu\n", (unsigned long)frame, (unsigned long)Doom_Tick());
        exit(0);
    }
}
void Doom_GuestPresent(const uint16_t *pixels, uint32_t frame) { present(pixels, NULL, frame); }
void Doom_GuestPresentIndexed(const unsigned char *pixels, const uint16_t *palette, uint32_t frame) {
    present((const uint16_t *)pixels, palette, frame);
}
