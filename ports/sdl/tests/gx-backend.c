/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Isolated backend checks: fake GX/SDL services, with no SDL renderer linked. */
#include "SDL.h"
#include "SDL_config.h"
#include "SDL_events_c.h"
#include "SDL_pixels_c.h"
#include "SDL_sysvideo.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <gx.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>
static int alloc_until_fail = -1, live_allocations, opens;
static const char *zoom_hint;
static int zoom_cap = 1, zoom_calls, zoom_request, zoom_effective, fail_zoom;
static unsigned configured_width, configured_height;
static void *test_malloc(size_t n) {
    void *p;
    if (alloc_until_fail == 0)
        return NULL;
    if (alloc_until_fail > 0)
        --alloc_until_fail;
    p = malloc(n);
    if (p)
        live_allocations++;
    return p;
}
static void test_free(void *p) {
    if (p)
        live_allocations--;
    free(p);
}
static int test_open(const char *p, int f, ...) {
    (void)p;
    (void)f;
    opens++;
    return 11;
}
static int closes, creates, destroys, presents, uploads, configs, fail_config, direct = 1, quits;
static int last_x, last_y, last_w, last_h, last_stride, last_req;
static unsigned long last_colour;
static struct gx_event queued[40];
static unsigned queue_count, queue_index;
static int test_close(int fd) {
    assert(fd == 11);
    closes++;
    return 0;
}
static int test_ioctl(int fd, unsigned long req, void *a) {
    assert(fd == 11);
    if (req == GX_INFO) {
        struct gx_info *i = a;
        i->max_width = 1920;
        i->max_height = 1080;
        i->flags = (direct ? GX_CAP_BGR888_UPLOAD : 0) | (zoom_cap ? GX_CAP_PRESENTATION_ZOOM : 0);
        return 0;
    }
    if (req == GX_FORMATS) {
        struct gx_formats *f = a;
        f->formats = GX_FORMAT_XRGB8888 | GX_FORMAT_RGB565;
        return 0;
    }
    if (req == GX_CREATE) {
        struct gx_window *w = a;
        w->window = 2;
        w->surface = 3;
        creates++;
        return 0;
    }
    if (req == GX_PRESENTATION_ZOOM) {
        struct gx_presentation *zoom = a;
        assert(zoom_cap && zoom->version == GX_VERSION && zoom->window == 2);
        assert(zoom->percent >= 100 && zoom->percent <= 300);
        zoom_calls++;
        zoom_request = zoom->percent;
        if (fail_zoom) {
            fail_zoom = 0;
            return -1;
        }
        if (zoom_effective)
            zoom->percent = zoom_effective;
        return 0;
    }
    if (req == GX_CONFIGURE) {
        struct gx_configure *configure = a;
        configs++;
        if (fail_config) {
            fail_config = 0;
            return -1;
        }
        configured_width = configure->width;
        configured_height = configure->height;
        return 0;
    }
    if (req == GX_DESTROY) {
        destroys++;
        return 0;
    }
    if (req == GX_PRESENT) {
        presents++;
        return 0;
    }
    if (req == GX_UPLOAD_BGR888 || req == GX_UPLOAD) {
        struct gx_upload *u = a;
        uploads++;
        last_x = u->x;
        last_y = u->y;
        last_w = u->width;
        last_h = u->height;
        last_stride = u->stride;
        last_req = req;
        if (req == GX_UPLOAD) {
            last_colour = u->pixels[0];
        } else {
            const unsigned char *p = (const void *)u->pixels;
            last_colour = (unsigned long)p[2] << 16 | p[1] << 8 | p[0];
        }
        return 0;
    }
    if (req == GX_EVENT) {
        struct gx_event *e = a;
        if (queue_index < queue_count)
            *e = queued[queue_index++];
        else
            e->type = 0;
        return 0;
    }
    if (req == GX_TITLE)
        return 0;
    assert(!"Unexpected ioctl");
    return -1;
}
static Uint32 fake_clock = 10000;
static int clock_until_fail = -1, select_calls, select_interrupts, select_ready;
static int test_clock_gettime(clockid_t id, struct timespec *now) {
    assert(id == CLOCK_MONOTONIC);
    if (clock_until_fail == 0) {
        errno = EIO;
        return -1;
    }
    if (clock_until_fail > 0)
        clock_until_fail--;
    now->tv_sec = fake_clock / 1000;
    now->tv_nsec = (fake_clock % 1000) * 1000000;
    return 0;
}
static int test_select(int nfds, fd_set *read_set, fd_set *write_set, fd_set *except_set,
                       struct timeval *timeout) {
    (void)nfds;
    (void)read_set;
    (void)write_set;
    (void)except_set;
    select_calls++;
    if (select_interrupts) {
        select_interrupts--;
        fake_clock++;
        errno = EINTR;
        return -1;
    }
    if (select_ready)
        return 1;
    fake_clock += timeout->tv_sec * 1000 + timeout->tv_usec / 1000;
    return 0;
}
static char *test_getenv(const char *name) {
    return !strcmp(name, "SDL_GX_WINDOW_ZOOM") ? (char *)zoom_hint : NULL;
}
#undef SDL_getenv
#define SDL_getenv test_getenv
#undef SDL_malloc
#undef SDL_free
#define SDL_malloc test_malloc
#define SDL_free test_free
#define open test_open
#define close test_close
#define ioctl test_ioctl
#define select test_select
#define clock_gettime test_clock_gettime
#include "../platform/gx.c"
#undef open
#undef close
#undef ioctl
#undef select
#undef clock_gettime
int SDL_TranslateUNICODE;
static Uint8 mouse_state;
static int mouse_x, mouse_y, resets, keys, buttons, resizes, exposes, actives;
static SDL_keysym observed[20];
static unsigned observed_buttons[30];
void SDL_SetError(const char *fmt, ...) { (void)fmt; }
void SDL_Error(SDL_errorcode n) { (void)n; }
int SDL_PrivateQuit(void) {
    quits++;
    return 1;
}
int SDL_PrivateKeyboard(Uint8 s, SDL_keysym *k) {
    assert(keys < 20);
    observed[keys++] = *k;
    (void)s;
    return 1;
}
Uint8 SDL_GetMouseState(int *x, int *y) {
    if (x)
        *x = mouse_x;
    if (y)
        *y = mouse_y;
    return mouse_state;
}
void SDL_ResetKeyboard(void) { resets++; }
int SDL_PrivateMouseButton(Uint8 state, Uint8 button, Sint16 x, Sint16 y) {
    (void)x;
    (void)y;
    observed_buttons[buttons++] = button;
    if (state)
        mouse_state |= SDL_BUTTON(button);
    else
        mouse_state &= ~SDL_BUTTON(button);
    return 1;
}
int SDL_PrivateMouseMotion(Uint8 b, int relative, Sint16 x, Sint16 y) {
    (void)b;
    assert(!relative);
    mouse_x = x;
    mouse_y = y;
    return 1;
}
int SDL_PrivateResize(int w, int h) {
    assert(w == 800 && h == 480);
    resizes++;
    return 1;
}
int SDL_PrivateExpose(void) {
    exposes++;
    return 1;
}
int SDL_PrivateAppActive(Uint8 gain, Uint8 state) {
    (void)gain;
    (void)state;
    actives++;
    return 1;
}
SDLMod SDL_GetModState(void) { return KMOD_NONE; }
SDL_PixelFormat *SDL_AllocFormat(int b, Uint32 r, Uint32 g, Uint32 blue, Uint32 a) {
    SDL_PixelFormat *f = test_malloc(sizeof(*f));
    if (f) {
        memset(f, 0, sizeof(*f));
        f->BitsPerPixel = b;
        f->BytesPerPixel = b / 8;
        f->Rmask = r;
        f->Gmask = g;
        f->Bmask = blue;
        f->Amask = a;
    }
    return f;
}
void SDL_FreeFormat(SDL_PixelFormat *f) { test_free(f); }
void SDL_FormatChanged(SDL_Surface *s) { s->format_version++; }
size_t SDL_strlcpy(char *d, const char *s, size_t n) {
    size_t l = strlen(s);
    if (n) {
        size_t c = l < n ? l : n - 1;
        memcpy(d, s, c);
        d[c] = 0;
    }
    return l;
}
static void enqueue(unsigned type, unsigned code, unsigned flags, long dx, long dy) {
    struct gx_event *e = &queued[queue_count++];
    memset(e, 0, sizeof(*e));
    e->type = type;
    e->code = code;
    e->flags = flags;
    e->dx = dx;
    e->dy = dy;
    e->x = 4;
    e->y = 5;
}
int main(void) {
    SDL_VideoDevice *d;
    SDL_PixelFormat display;
    SDL_Surface s;
    void *old;
    SDL_PixelFormat *old_format;
    int base;
    memset(&s, 0, sizeof(s));
    d = GX_CreateDevice(0);
    assert(d && GX_VideoInit(d, &display) == 0);
    d->screen = &s;
    assert(display.BitsPerPixel == 24);
    assert(d->hidden->window.format == GX_XRGB8888);
    s.format = SDL_AllocFormat(24, 0, 0, 0, 0);
    assert(GX_SetVideoMode(d, &s, 640, 400, 32, SDL_RESIZABLE) == &s);
    assert(s.pitch == 1920 && s.format->BytesPerPixel == 3);
    assert(creates == 1);
    /* Monotonic deadlines, interruption and clock failure never busy-spin. */
    SDL_GX_StartTicks();
    assert(SDL_GX_GetTicks() == 0);
    fake_clock += 2005;
    assert(SDL_GX_GetTicks() == 2005);
    SDL_GX_Delay(25);
    assert(SDL_GX_GetTicks() == 2030);
    base = select_calls;
    assert(SDL_GX_Wait(25) == 0 && select_calls == base + 1);
    select_ready = 1;
    assert(SDL_GX_Wait(25) == 1);
    select_ready = 0;
    select_interrupts = 1;
    base = select_calls;
    assert(SDL_GX_Wait(25) == 0 && select_calls == base + 2);
    clock_until_fail = 0;
    base = select_calls;
    assert(SDL_GX_Wait(25) == -1 && select_calls == base);
    SDL_GX_Delay(25);
    assert(select_calls == base);
    clock_until_fail = 1;
    select_interrupts = 1;
    assert(SDL_GX_Wait(25) == -1 && select_calls == base + 1);
    clock_until_fail = -1;
    old = s.pixels;
    old_format = s.format;
    base = live_allocations;
    alloc_until_fail = 0;
    assert(!GX_SetVideoMode(d, &s, 800, 480, 32, SDL_RESIZABLE));
    alloc_until_fail = -1;
    assert(s.pixels == old && s.format == old_format && base == live_allocations);
    alloc_until_fail = 1;
    assert(!GX_SetVideoMode(d, &s, 800, 480, 32, SDL_RESIZABLE));
    alloc_until_fail = -1;
    assert(s.pixels == old && s.format == old_format && base == live_allocations);
    fail_config = 1;
    assert(!GX_SetVideoMode(d, &s, 800, 480, 32, SDL_RESIZABLE));
    assert(s.pixels == old && s.format == old_format && base == live_allocations &&
           d->hidden->window.width == 640);
    assert(!GX_SetVideoMode(d, &s, 1921, 1080, 24, 0));
    assert(!GX_SetVideoMode(d, &s, 640, 400, 24, SDL_FULLSCREEN));
    assert(base == live_allocations);
    ((Uint8 *)s.pixels)[0] = 0x12;
    ((Uint8 *)s.pixels)[1] = 0x34;
    ((Uint8 *)s.pixels)[2] = 0x56;
    {
        SDL_Rect r[2] = {{-5, -5, 10, 10}, {639, 399, 10, 10}};
        GX_UpdateRects(d, 2, r);
        assert(uploads == 2 && presents == 1 && last_x == 639 && last_y == 399 && last_w == 1 &&
               last_h == 1 && last_stride == 1920);
    }
    SDL_TranslateUNICODE = 1;
    enqueue(GX_KEY, 4, GX_DOWN | GX_TEXT_KEY, 0, 0);
    enqueue(GX_TEXT, 0, 0, 0x416, 0);
    enqueue(GX_KEY, 4, 0, 0, 0);
    enqueue(GX_KEY, 79, GX_DOWN, 0, 0);
    enqueue(GX_RESIZE, 0, 0, 0, 0);
    queued[queue_count - 1].x = 800;
    queued[queue_count - 1].y = 480;
    GX_PumpEvents(d);
    assert(keys == 3 && observed[0].sym == SDLK_a && observed[0].unicode == 0x416 &&
           observed[1].unicode == 0 && observed[2].sym == SDLK_RIGHT && resizes == 1 &&
           exposes == 1);
    queue_count = queue_index = 0;
    enqueue(GX_BUTTON, 0, GX_DOWN, 0, 0);
    queued[0].buttons = 1;
    enqueue(GX_FOCUS, 0, 0, 0, 0);
    GX_PumpEvents(d);
    assert(!mouse_state && buttons == 2 && observed_buttons[0] == SDL_BUTTON_LEFT &&
           observed_buttons[1] == SDL_BUTTON_LEFT);
    queue_count = queue_index = 0;
    buttons = 0;
    enqueue(GX_WHEEL, 0, 0, 0, 0);
    queued[0].wheel_y = 32768;
    enqueue(GX_WHEEL, 0, 0, 0, 0);
    queued[1].wheel_y = 32768;
    queued[1].wheel_x = 65536;
    GX_PumpEvents(d);
    assert(buttons == 4 && observed_buttons[0] == 4 && observed_buttons[2] == 7);
    GX_VideoQuit(d);
    assert(!s.pixels && closes == 1 && destroys == 1 && !gx_device);
    SDL_FreeFormat(s.format);
    s.format = NULL;
    GX_DeleteDevice(d);
    assert(!live_allocations && closes == 1);
    assert(zoom_calls == 0); /* Other applications keep existing defaults. */
    direct = 0;
    zoom_hint = "150";
    zoom_cap = 0;
    memset(&s, 0, sizeof(s));
    d = GX_CreateDevice(0);
    assert(d && GX_VideoInit(d, &display) == 0);
    d->screen = &s;
    s.format = SDL_AllocFormat(24, 0, 0, 0, 0);
    assert(GX_SetVideoMode(d, &s, 20, 40, 24, 0));
    memset(s.pixels, 255, (unsigned long)s.pitch * s.h);
    uploads = presents = 0;
    {
        SDL_Rect r = {0, 0, 20, 40};
        GX_UpdateRects(d, 1, &r);
        assert(uploads == 2 && presents == 1 && last_req == GX_UPLOAD && last_h == 8 &&
               last_y == 32 && last_colour == 65535);
    }
    old = s.pixels;
    old_format = s.format;
    base = live_allocations;
    alloc_until_fail = 1;
    assert(!GX_SetVideoMode(d, &s, 30, 40, 24, 0));
    alloc_until_fail = -1;
    assert(old == s.pixels && old_format == s.format && base == live_allocations);
    GX_VideoQuit(d);
    SDL_FreeFormat(s.format);
    s.format = NULL;
    GX_DeleteDevice(d);
    assert(!live_allocations && closes == 2 && destroys == 2);
    assert(zoom_calls == 0); /* Older GX backends remain usable. */

    /* Presentation zoom preserves the SDL raster, pointer and resize geometry. */
    direct = zoom_cap = 1;
    zoom_effective = 125; /* Host fit may be smaller than the requested 150%. */
    memset(&s, 0, sizeof(s));
    d = GX_CreateDevice(0);
    assert(d && GX_VideoInit(d, &display) == 0);
    d->screen = &s;
    s.format = SDL_AllocFormat(24, 0, 0, 0, 0);
    assert(GX_SetVideoMode(d, &s, 640, 400, 24, SDL_RESIZABLE));
    assert(zoom_calls == 1 && zoom_request == 150 && d->hidden->zoom_percent == 150);
    assert(s.w == 640 && s.h == 400 && s.pitch == 1920);
    assert(configured_width == 640 && configured_height == 400);
    queue_count = queue_index = 0;
    enqueue(GX_MOTION, 0, 0, 0, 0);
    queued[0].x = 123;
    queued[0].y = 234;
    enqueue(GX_RESIZE, 0, 0, 0, 0);
    queued[1].x = 800;
    queued[1].y = 480;
    GX_PumpEvents(d);
    assert(mouse_x == 123 && mouse_y == 234 && resizes == 2);
    assert(GX_SetVideoMode(d, &s, 800, 480, 24, SDL_RESIZABLE));
    assert(zoom_calls == 2 && zoom_request == 150 && d->hidden->zoom_percent == 150);
    assert(s.w == 800 && s.h == 480 && s.pitch == 2400);
    old = s.pixels;
    old_format = s.format;
    base = live_allocations;
    fail_zoom = 1;
    assert(!GX_SetVideoMode(d, &s, 640, 400, 24, SDL_RESIZABLE));
    assert(zoom_calls == 3 && old == s.pixels && old_format == s.format &&
           base == live_allocations);
    assert(configured_width == 800 && configured_height == 480);
    GX_VideoQuit(d);
    SDL_FreeFormat(s.format);
    s.format = NULL;
    GX_DeleteDevice(d);
    assert(!live_allocations && closes == 3 && destroys == 3);

    /* Malformed values fail before opening a descriptor or creating a window. */
    {
        static const char *invalid[] = {"",     "99",   "301",
                                        "150x", "1.5",  "-150",
                                        "+150", " 150", "999999999999999999999999999999999999"};
        unsigned i;
        base = opens;
        for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
            zoom_hint = invalid[i];
            d = GX_CreateDevice(0);
            assert(d && GX_VideoInit(d, &display) == -1);
            assert(d->hidden->fd == -1 && opens == base);
            GX_DeleteDevice(d);
            assert(!live_allocations);
        }
    }
    zoom_hint = NULL;
    puts("SDL_GX_MOCK_OK");
    return 0;
}
