/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* SDL 1.2 software-video driver for the GX device contract.
 *
 * The native SDL surface is B,G,R, one logical octet per C character. GX
 * marshals that representation synchronously; neither the driver nor SDL
 * relies on host framebuffer packing. Requested depths are emulated by the
 * upstream SDL shadow surface. No hardware surface, fullscreen, OpenGL,
 * relative input capture, or native cursor customization is advertised.
 */
#include "SDL_config.h"
#include "SDL_events_c.h"
#include "SDL_keyboard.h"
#include "SDL_mouse.h"
#include "SDL_pixels_c.h"
#include "SDL_sysvideo.h"
#include "SDL_video.h"
#include "platform.h"
#include <errno.h>
#include <fcntl.h>
#include <gx.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#define GX_SDL_MAX_WIDTH 1920
#define GX_SDL_MAX_HEIGHT 1080
#define GX_SDL_CONVERT_ROWS 32
#define GX_INPUT_INSIDE 4

struct SDL_PrivateVideoData {
    int fd;
    int direct_upload;
    int failed;
    int profiling;
    int presentation_zoom;
    unsigned zoom_percent;
    unsigned max_width, max_height;
    unsigned buttons;
    Sint32 wheel_x, wheel_y;
    struct gx_window window;
    Uint8 *buffer;
    Uint16 *converted;
    SDL_keysym pending;
    int pending_key;
    Uint32 mode_flags;
};

#define _THIS SDL_VideoDevice *_this
static SDL_VideoDevice *gx_device;
static Uint32 clock_epoch;
static int clock_started;
static int clock_failed;

static Uint32 microseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0;
    return (Uint32)now.tv_sec * 1000000UL + (Uint32)now.tv_nsec / 1000UL;
}

static Uint32 milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        clock_failed = 1;
        SDL_SetError("GX: cannot read monotonic clock");
        return 0;
    }
    clock_failed = 0;
    return (Uint32)now.tv_sec * 1000UL + (Uint32)now.tv_nsec / 1000000UL;
}

void SDL_GX_StartTicks(void) {
    clock_epoch = milliseconds();
    clock_started = !clock_failed;
}

Uint32 SDL_GX_GetTicks(void) {
    Uint32 now;
    if (!clock_started)
        SDL_GX_StartTicks();
    if (!clock_started)
        return 0;
    now = milliseconds();
    return clock_failed ? 0 : now - clock_epoch;
}

void SDL_GX_Delay(Uint32 delay) {
    Uint32 start, elapsed;
    struct timeval timeout;
    int result;
    while (delay) {
        /* Short bounded waits also avoid overflowing time_t on C16. */
        Uint32 part = delay > 1000UL ? 1000UL : delay;
        timeout.tv_sec = part / 1000UL;
        timeout.tv_usec = (part % 1000UL) * 1000UL;
        start = milliseconds();
        if (clock_failed)
            return;
        result = select(0, NULL, NULL, NULL, &timeout);
        if (result == 0) {
            delay -= part;
        } else {
            if (result < 0 && errno != EINTR) {
                SDL_SetError("GX: delay failed");
                return;
            }
            elapsed = milliseconds() - start;
            if (clock_failed)
                return;
            if (elapsed >= delay)
                return;
            delay -= elapsed;
        }
    }
}

int SDL_GX_Wait(Uint32 delay) {
    struct timeval timeout;
    fd_set read_set;
    int result, fd;
    Uint32 start, elapsed;
    if (!gx_device || gx_device->hidden->fd < 0 || !gx_device->hidden->window.window) {
        SDL_GX_Delay(delay);
        return 0;
    }
    fd = gx_device->hidden->fd;
    if (fd >= FD_SETSIZE) {
        SDL_SetError("GX: graphics descriptor exceeds select capacity");
        return -1;
    }
    for (;;) {
        Uint32 part = delay > 1000UL ? 1000UL : delay;
        timeout.tv_sec = part / 1000UL;
        timeout.tv_usec = (part % 1000UL) * 1000UL;
        FD_ZERO(&read_set);
        FD_SET(fd, &read_set);
        start = milliseconds();
        if (clock_failed)
            return -1;
        result = select(fd + 1, &read_set, NULL, NULL, &timeout);
        if (result > 0)
            return 1;
        if (result < 0 && errno != EINTR) {
            SDL_SetError("GX: input wait failed");
            return -1;
        }
        elapsed = result == 0 ? part : milliseconds() - start;
        if (clock_failed)
            return -1;
        if (elapsed >= delay)
            return 0;
        delay -= elapsed;
    }
}

static void GX_Failed(_THIS, const char *message) {
    SDL_SetError("GX: %s", message);
    if (!_this->hidden->failed) {
        _this->hidden->failed = 1;
        SDL_PrivateQuit();
    }
}

static void GX_FlushKey(_THIS, Uint32 text) {
    struct SDL_PrivateVideoData *p = _this->hidden;
    if (!p->pending_key)
        return;
    if (SDL_TranslateUNICODE)
        p->pending.unicode = text > 0xffffUL ? 0xfffdU : (Uint16)text;
    SDL_PrivateKeyboard(SDL_PRESSED, &p->pending);
    p->pending_key = 0;
}

static void GX_ReleaseInputs(_THIS) {
    unsigned button;
    Uint8 state;
    GX_FlushKey(_this, 0);
    SDL_ResetKeyboard();
    /* Avoid shifting by a negative count for button zero in SDL_ResetMouse. */
    state = SDL_GetMouseState(NULL, NULL);
    for (button = 1; button <= 7; button++)
        if (state & SDL_BUTTON(button))
            SDL_PrivateMouseButton(SDL_RELEASED, button, 0, 0);
    _this->hidden->buttons = 0;
    _this->hidden->wheel_x = _this->hidden->wheel_y = 0;
}

static int GX_Available(void) { return 1; }

static int GX_ReadZoom(_THIS) {
    const char *hint = SDL_getenv("SDL_GX_WINDOW_ZOOM");
    unsigned value = 0;
    _this->hidden->zoom_percent = 0;
    if (!hint)
        return 0;
    if (!*hint)
        goto invalid;
    while (*hint) {
        if (*hint < '0' || *hint > '9')
            goto invalid;
        value = value * 10U + (*hint++ - '0');
        if (value > 300U)
            goto invalid;
    }
    if (value < 100U)
        goto invalid;
    _this->hidden->zoom_percent = value;
    return 0;
invalid:
    SDL_SetError("SDL_GX_WINDOW_ZOOM must be an integer from 100 to 300");
    return -1;
}

static int GX_ApplyZoom(_THIS, Uint32 window) {
    struct SDL_PrivateVideoData *p = _this->hidden;
    struct gx_presentation zoom;
    /* An absent hint preserves the platform's existing presentation policy.
     * Older GX backends keep their ordinary 1x presentation. */
    if (!p->zoom_percent || !p->presentation_zoom)
        return 0;
    SDL_memset(&zoom, 0, sizeof(zoom));
    zoom.version = GX_VERSION;
    zoom.window = window;
    zoom.percent = p->zoom_percent;
    return ioctl(p->fd, GX_PRESENTATION_ZOOM, &zoom);
}

static int GX_VideoInit(_THIS, SDL_PixelFormat *format) {
    struct gx_info info;
    struct gx_formats formats;
    struct SDL_PrivateVideoData *p = _this->hidden;
    if (GX_ReadZoom(_this) < 0)
        return -1;
    p->profiling = SDL_getenv("SDL_PROFILE") != NULL;
    p->fd = open("/dev/gfx", O_RDWR | O_CLOEXEC);
    if (p->fd < 0) {
        SDL_SetError("GX: cannot open /dev/gfx");
        return -1;
    }
    SDL_memset(&info, 0, sizeof(info));
    info.version = GX_VERSION;
    if (ioctl(p->fd, GX_INFO, &info) < 0 || !info.max_width || !info.max_height) {
        SDL_SetError("GX: cannot query graphics capabilities");
        close(p->fd);
        p->fd = -1;
        return -1;
    }
    p->max_width = info.max_width < GX_SDL_MAX_WIDTH ? info.max_width : GX_SDL_MAX_WIDTH;
    p->max_height = info.max_height < GX_SDL_MAX_HEIGHT ? info.max_height : GX_SDL_MAX_HEIGHT;
    p->direct_upload = (info.flags & GX_CAP_BGR888_UPLOAD) != 0;
    p->presentation_zoom = (info.flags & GX_CAP_PRESENTATION_ZOOM) != 0;
    p->window.format = GX_RGB565;
    SDL_memset(&formats, 0, sizeof(formats));
    formats.version = GX_VERSION;
    if (p->direct_upload && ioctl(p->fd, GX_FORMATS, &formats) == 0 &&
        (formats.formats & GX_FORMAT_XRGB8888))
        p->window.format = GX_XRGB8888;
    format->BitsPerPixel = 24;
    format->BytesPerPixel = 3;
    format->Rmask = 0xff0000UL;
    format->Gmask = 0x00ff00UL;
    format->Bmask = 0x0000ffUL;
    _this->info.wm_available = 1;
    _this->info.current_w = p->max_width < 640 ? p->max_width : 640;
    _this->info.current_h = p->max_height < 480 ? p->max_height : 480;
    _this->handles_any_size = 1;
    gx_device = _this;
    return 0;
}

static SDL_Rect **GX_ListModes(_THIS, SDL_PixelFormat *format, Uint32 flags) {
    (void)_this;
    if ((flags & (SDL_OPENGL | SDL_FULLSCREEN | SDL_NOFRAME)) || format->BitsPerPixel != 24)
        return NULL;
    return (SDL_Rect **)-1;
}

static SDL_Surface *GX_SetVideoMode(_THIS, SDL_Surface *surface, int width, int height, int bpp,
                                    Uint32 flags) {
    struct SDL_PrivateVideoData *p = _this->hidden;
    struct gx_window next;
    struct gx_configure configure;
    struct gx_handle handle;
    Uint8 *buffer;
    Uint16 *converted = NULL;
    SDL_PixelFormat *format;
    unsigned long pitch, cells;
    int created = 0, configured = 0;
    (void)bpp;
    if (flags & (SDL_OPENGL | SDL_FULLSCREEN | SDL_NOFRAME)) {
        SDL_SetError("GX: only framed windowed software video is supported");
        return NULL;
    }
    if (width <= 0 || height <= 0 || (unsigned)width > p->max_width ||
        (unsigned)height > p->max_height) {
        SDL_SetError("GX: invalid or unsupported window dimensions");
        return NULL;
    }
    pitch = (unsigned long)width * 3UL;
    /* SDL_Surface::pitch and GX strides are 16-bit logical-cell counts. */
    if (pitch > 65535UL) {
        SDL_SetError("GX: surface pitch exceeds 65535 cells");
        return NULL;
    }
    cells = pitch * (unsigned long)height;
    buffer = SDL_malloc(cells * sizeof(*buffer));
    if (!p->direct_upload)
        converted = SDL_malloc((unsigned long)width * GX_SDL_CONVERT_ROWS * sizeof(*converted));
    if (!buffer || (!p->direct_upload && !converted)) {
        SDL_free(buffer);
        SDL_free(converted);
        SDL_OutOfMemory();
        return NULL;
    }
    SDL_memset(buffer, 0, cells * sizeof(*buffer));
    /* SDL_ReallocFormat frees the previous format before allocating. Keep
     * mode changes atomic when allocation fails instead. */
    format = SDL_AllocFormat(24, 0xff0000UL, 0x00ff00UL, 0x0000ffUL, 0);
    if (!format) {
        SDL_free(buffer);
        SDL_free(converted);
        return NULL;
    }
    next = p->window;
    if (!next.window) {
        next.version = GX_VERSION;
        next.width = width;
        next.height = height;
        SDL_strlcpy(next.title, _this->wm_title ? _this->wm_title : "SDL", sizeof(next.title));
        if (ioctl(p->fd, GX_CREATE, &next) < 0)
            goto fail;
        created = 1;
    }
    SDL_memset(&configure, 0, sizeof(configure));
    configure.version = GX_VERSION;
    configure.window = next.window;
    configure.width = width;
    configure.height = height;
    configure.flags = (flags & SDL_RESIZABLE) ? 0 : GX_WINDOW_FIXED;
    if (ioctl(p->fd, GX_CONFIGURE, &configure) < 0)
        goto fail;
    configured = 1;
    if (GX_ApplyZoom(_this, next.window) < 0)
        goto fail;
    SDL_FreeFormat(surface->format);
    surface->format = format;
    SDL_FormatChanged(surface);
    SDL_free(p->buffer);
    SDL_free(p->converted);
    p->buffer = buffer;
    p->converted = converted;
    p->window = next;
    p->window.width = width;
    p->window.height = height;
    p->mode_flags = flags & SDL_RESIZABLE;
    p->failed = 0;
    surface->flags = p->mode_flags;
    surface->w = width;
    surface->h = height;
    surface->pitch = pitch;
    surface->pixels = buffer;
    GX_ReleaseInputs(_this);
    return surface;
fail:
    if (configured && !created) {
        configure.width = p->window.width;
        configure.height = p->window.height;
        configure.flags = (p->mode_flags & SDL_RESIZABLE) ? 0 : GX_WINDOW_FIXED;
        ioctl(p->fd, GX_CONFIGURE, &configure);
    }
    if (created) {
        handle.version = GX_VERSION;
        handle.handle = next.window;
        ioctl(p->fd, GX_DESTROY, &handle);
    }
    SDL_FreeFormat(format);
    SDL_free(buffer);
    SDL_free(converted);
    SDL_SetError("GX: cannot configure video mode");
    return NULL;
}

static void GX_UpdateRects(_THIS, int count, SDL_Rect *rects) {
    struct SDL_PrivateVideoData *p = _this->hidden;
    struct gx_upload upload;
    struct gx_handle handle;
    Uint32 started = 0, upload_us = 0, convert_us = 0, present_us = 0;
    Uint32 pixels = 0, rectangles = 0;
    int i, changed = 0;
    if (!p->buffer || !p->window.window || p->failed)
        return;
    for (i = 0; i < count; i++) {
        long x = rects[i].x, y = rects[i].y;
        long right = x + (long)rects[i].w;
        long bottom = y + (long)rects[i].h;
        unsigned rows, row, col;
        const Uint8 *source;
        if (x < 0)
            x = 0;
        if (y < 0)
            y = 0;
        if (right > p->window.width)
            right = p->window.width;
        if (bottom > p->window.height)
            bottom = p->window.height;
        if (x >= right || y >= bottom)
            continue;
        SDL_memset(&upload, 0, sizeof(upload));
        upload.version = GX_VERSION;
        upload.surface = p->window.surface;
        upload.x = x;
        upload.y = y;
        upload.width = right - x;
        upload.height = bottom - y;
        source = p->buffer + (unsigned long)y * _this->screen->pitch + (unsigned long)x * 3UL;
        if (p->profiling) {
            pixels += (Uint32)upload.width * upload.height;
            rectangles++;
        }
        if (p->direct_upload) {
            upload.stride = _this->screen->pitch;
            upload.pixels = (const Uint16 *)source;
            if (p->profiling)
                started = microseconds();
            if (ioctl(p->fd, GX_UPLOAD_BGR888, &upload) < 0) {
                GX_Failed(_this, "BGR upload failed");
                return;
            }
            if (p->profiling)
                upload_us += microseconds() - started;
        } else {
            upload.stride = upload.width;
            upload.pixels = p->converted;
            while (upload.y < bottom) {
                rows = bottom - upload.y;
                if (rows > GX_SDL_CONVERT_ROWS)
                    rows = GX_SDL_CONVERT_ROWS;
                if (p->profiling)
                    started = microseconds();
                for (row = 0; row < rows; row++) {
                    const Uint8 *pixel = source + (unsigned long)row * _this->screen->pitch;
                    Uint16 *dest = p->converted + (unsigned long)row * upload.width;
                    for (col = 0; col < upload.width; col++, pixel += 3)
                        dest[col] = ((pixel[2] & 248U) << 8) | ((pixel[1] & 252U) << 3) |
                                    ((pixel[0] & 255U) >> 3);
                }
                upload.height = rows;
                if (p->profiling) {
                    convert_us += microseconds() - started;
                    started = microseconds();
                }
                if (ioctl(p->fd, GX_UPLOAD, &upload) < 0) {
                    GX_Failed(_this, "RGB565 upload failed");
                    return;
                }
                if (p->profiling)
                    upload_us += microseconds() - started;
                upload.y += rows;
                source += (unsigned long)rows * _this->screen->pitch;
            }
        }
        changed = 1;
    }
    if (changed) {
        handle.version = GX_VERSION;
        handle.handle = p->window.window;
        if (p->profiling)
            started = microseconds();
        if (ioctl(p->fd, GX_PRESENT, &handle) < 0)
            GX_Failed(_this, "present failed");
        if (p->profiling) {
            present_us = microseconds() - started;
            printf("SDL_GX_PROFILE size=%ux%u rects=%lu pixels=%lu convert_us=%lu upload_us=%lu "
                   "present_us=%lu direct=%d format=%u\n",
                   p->window.width, p->window.height, (unsigned long)rectangles,
                   (unsigned long)pixels, (unsigned long)convert_us, (unsigned long)upload_us,
                   (unsigned long)present_us, p->direct_upload, p->window.format);
            fflush(stdout);
        }
    }
}

static SDLKey GX_Key(unsigned code) {
    static const SDLKey special[] = {
        SDLK_RETURN, SDLK_ESCAPE,    SDLK_BACKSPACE,   SDLK_TAB,          SDLK_SPACE,
        SDLK_MINUS,  SDLK_EQUALS,    SDLK_LEFTBRACKET, SDLK_RIGHTBRACKET, SDLK_BACKSLASH,
        SDLK_HASH,   SDLK_SEMICOLON, SDLK_QUOTE,       SDLK_BACKQUOTE,    SDLK_COMMA,
        SDLK_PERIOD, SDLK_SLASH,     SDLK_CAPSLOCK};
    if (code >= 4 && code <= 29)
        return (SDLKey)(SDLK_a + code - 4);
    if (code >= 30 && code <= 38)
        return (SDLKey)(SDLK_1 + code - 30);
    if (code == 39)
        return SDLK_0;
    if (code >= 40 && code <= 57)
        return special[code - 40];
    if (code >= 58 && code <= 69)
        return (SDLKey)(SDLK_F1 + code - 58);
    if (code >= 89 && code <= 97)
        return (SDLKey)(SDLK_KP1 + code - 89);
    if (code >= 104 && code <= 106)
        return (SDLKey)(SDLK_F13 + code - 104);
    switch (code) {
    case 70:
        return SDLK_PRINT;
    case 71:
        return SDLK_SCROLLOCK;
    case 72:
        return SDLK_PAUSE;
    case 73:
        return SDLK_INSERT;
    case 74:
        return SDLK_HOME;
    case 75:
        return SDLK_PAGEUP;
    case 76:
        return SDLK_DELETE;
    case 77:
        return SDLK_END;
    case 78:
        return SDLK_PAGEDOWN;
    case 79:
        return SDLK_RIGHT;
    case 80:
        return SDLK_LEFT;
    case 81:
        return SDLK_DOWN;
    case 82:
        return SDLK_UP;
    case 83:
        return SDLK_NUMLOCK;
    case 84:
        return SDLK_KP_DIVIDE;
    case 85:
        return SDLK_KP_MULTIPLY;
    case 86:
        return SDLK_KP_MINUS;
    case 87:
        return SDLK_KP_PLUS;
    case 88:
        return SDLK_KP_ENTER;
    case 98:
        return SDLK_KP0;
    case 99:
        return SDLK_KP_PERIOD;
    case 100:
        return SDLK_BACKSLASH;
    case 101:
        return SDLK_MENU;
    case 102:
        return SDLK_POWER;
    case 103:
        return SDLK_KP_EQUALS;
    case 224:
        return SDLK_LCTRL;
    case 225:
        return SDLK_LSHIFT;
    case 226:
        return SDLK_LALT;
    case 227:
        return SDLK_LMETA;
    case 228:
        return SDLK_RCTRL;
    case 229:
        return SDLK_RSHIFT;
    case 230:
        return SDLK_RALT;
    case 231:
        return SDLK_RMETA;
    default:
        return SDLK_UNKNOWN;
    }
}

static Uint16 GX_ASCII(SDLKey key) {
    SDLMod mods = SDL_GetModState();
    const char *plain = "1234567890-=[]\\;'`,./";
    const char *shifted = "!@#$%^&*()_+{}|:\"~<>?";
    const char *at;
    if (key >= SDLK_a && key <= SDLK_z) {
        if (mods & KMOD_CTRL)
            return key - SDLK_a + 1;
        return ((mods & KMOD_SHIFT) != 0) != ((mods & KMOD_CAPS) != 0) ? key - 32 : key;
    }
    if (key >= SDLK_KP0 && key <= SDLK_KP9)
        return (mods & KMOD_NUM) ? '0' + key - SDLK_KP0 : 0;
    if (key == SDLK_KP_ENTER)
        return '\r';
    if (key == SDLK_KP_PERIOD)
        return (mods & KMOD_NUM) ? '.' : 0;
    if (key == SDLK_KP_DIVIDE)
        return '/';
    if (key == SDLK_KP_MULTIPLY)
        return '*';
    if (key == SDLK_KP_MINUS)
        return '-';
    if (key == SDLK_KP_PLUS)
        return '+';
    if (key == SDLK_KP_EQUALS)
        return '=';
    if (key >= 8 && key < 127) {
        at = SDL_strchr(plain, key);
        if ((mods & KMOD_SHIFT) && at)
            return shifted[at - plain];
        return key;
    }
    return 0;
}

static unsigned GX_MouseButtons(unsigned raw) {
    return ((raw & 1U) ? SDL_BUTTON(SDL_BUTTON_LEFT) : 0) |
           ((raw & 2U) ? SDL_BUTTON(SDL_BUTTON_RIGHT) : 0) |
           ((raw & 4U) ? SDL_BUTTON(SDL_BUTTON_MIDDLE) : 0);
}

static void GX_Mouse(_THIS, const struct gx_event *event) {
    struct SDL_PrivateVideoData *p = _this->hidden;
    unsigned current = GX_MouseButtons(event->buttons), button;
    Sint16 x = event->x < 0 ? 0 : event->x >= p->window.width ? p->window.width - 1 : event->x;
    Sint16 y = event->y < 0 ? 0 : event->y >= p->window.height ? p->window.height - 1 : event->y;
    SDL_PrivateMouseMotion(current, 0, x, y);
    for (button = SDL_BUTTON_LEFT; button <= SDL_BUTTON_RIGHT; button++)
        if ((current ^ p->buttons) & SDL_BUTTON(button))
            SDL_PrivateMouseButton((current & SDL_BUTTON(button)) ? SDL_PRESSED : SDL_RELEASED,
                                   button, 0, 0);
    p->buttons = current;
}

static void GX_WheelAxis(Sint32 *remainder, Sint32 delta, unsigned positive, unsigned negative) {
    unsigned button;
    /* A bounded event cannot overflow the accumulator or flood SDL's queue. */
    if (delta > 1048576L)
        delta = 1048576L;
    if (delta < -1048576L)
        delta = -1048576L;
    *remainder += delta;
    while (*remainder >= 65536L || *remainder <= -65536L) {
        if (*remainder > 0) {
            button = positive;
            *remainder -= 65536L;
        } else {
            button = negative;
            *remainder += 65536L;
        }
        SDL_PrivateMouseButton(SDL_PRESSED, button, 0, 0);
        SDL_PrivateMouseButton(SDL_RELEASED, button, 0, 0);
    }
}

static void GX_PumpEvents(_THIS) {
    struct SDL_PrivateVideoData *p = _this->hidden;
    struct gx_event event;
    unsigned count;
    if (!p->window.window || p->failed)
        return;
    /* Input floods must not starve the application's drawing and timers. */
    for (count = 0; count < 128; count++) {
        SDL_keysym key;
        SDL_memset(&event, 0, sizeof(event));
        event.version = GX_VERSION;
        event.window = p->window.window;
        if (ioctl(p->fd, GX_EVENT, &event) < 0) {
            GX_Failed(_this, "input read failed");
            break;
        }
        if (!event.type)
            break;
        if (event.type != GX_TEXT)
            GX_FlushKey(_this, 0);
        switch (event.type) {
        case GX_KEY:
            SDL_memset(&key, 0, sizeof(key));
            key.scancode = event.code;
            key.sym = GX_Key(event.code);
            if (event.flags & GX_DOWN) {
                if (event.flags & GX_TEXT_KEY) {
                    p->pending = key;
                    p->pending_key = 1;
                } else {
                    if (SDL_TranslateUNICODE)
                        key.unicode = GX_ASCII(key.sym);
                    SDL_PrivateKeyboard(SDL_PRESSED, &key);
                }
            } else {
                SDL_PrivateKeyboard(SDL_RELEASED, &key);
            }
            break;
        case GX_TEXT:
            if (p->pending_key) {
                GX_FlushKey(_this, (Uint32)event.dx);
            } else if (SDL_TranslateUNICODE) {
                /* SDL 1.2 has a single UCS-2 field, not a UTF-8 text event. */
                SDL_memset(&key, 0, sizeof(key));
                key.sym = SDLK_UNKNOWN;
                key.unicode = (Uint32)event.dx > 0xffffUL ? 0xfffdU : (Uint16)event.dx;
                SDL_PrivateKeyboard(SDL_PRESSED, &key);
                key.unicode = 0;
                SDL_PrivateKeyboard(SDL_RELEASED, &key);
            }
            break;
        case GX_MOTION:
        case GX_BUTTON:
            GX_Mouse(_this, &event);
            break;
        case GX_WHEEL:
            GX_Mouse(_this, &event);
            GX_WheelAxis(&p->wheel_y, event.wheel_y, SDL_BUTTON_WHEELUP, SDL_BUTTON_WHEELDOWN);
            GX_WheelAxis(&p->wheel_x, event.wheel_x, 7, 6);
            break;
        case GX_ENTER:
            SDL_PrivateAppActive((event.flags & GX_INPUT_INSIDE) != 0, SDL_APPMOUSEFOCUS);
            break;
        case GX_FOCUS:
            if (!(event.flags & GX_FOCUSED))
                GX_ReleaseInputs(_this);
            SDL_PrivateAppActive((event.flags & GX_FOCUSED) != 0, SDL_APPINPUTFOCUS);
            if (event.flags & GX_FOCUSED)
                SDL_PrivateExpose();
            break;
        case GX_OVERFLOW:
            GX_ReleaseInputs(_this);
            break;
        case GX_RESIZE:
            if ((p->mode_flags & SDL_RESIZABLE) && event.x > 0 && event.y > 0 &&
                (unsigned long)event.x <= p->max_width && (unsigned long)event.y <= p->max_height)
                SDL_PrivateResize(event.x, event.y);
            else {
                struct gx_configure configure;
                SDL_memset(&configure, 0, sizeof(configure));
                configure.version = GX_VERSION;
                configure.window = p->window.window;
                configure.width = p->window.width;
                configure.height = p->window.height;
                configure.flags = (p->mode_flags & SDL_RESIZABLE) ? 0 : GX_WINDOW_FIXED;
                if (ioctl(p->fd, GX_CONFIGURE, &configure) < 0 ||
                    GX_ApplyZoom(_this, p->window.window) < 0)
                    GX_Failed(_this, "resize acknowledgement failed");
            }
            SDL_PrivateExpose();
            break;
        case GX_CLOSE:
            GX_ReleaseInputs(_this);
            SDL_PrivateQuit();
            return;
        default:
            break;
        }
    }
    GX_FlushKey(_this, 0);
}

static void GX_InitOSKeymap(_THIS) { (void)_this; }

static void GX_SetCaption(_THIS, const char *title, const char *icon) {
    struct gx_title caption;
    (void)icon;
    if (!_this->hidden->window.window)
        return;
    SDL_memset(&caption, 0, sizeof(caption));
    caption.version = GX_VERSION;
    caption.window = _this->hidden->window.window;
    SDL_strlcpy(caption.title, title ? title : "SDL", sizeof(caption.title));
    if (ioctl(_this->hidden->fd, GX_TITLE, &caption) < 0)
        GX_Failed(_this, "title update failed");
}

static int GX_AllocHWSurface(_THIS, SDL_Surface *surface) {
    (void)_this;
    (void)surface;
    SDL_SetError("GX: hardware surfaces are unsupported");
    return -1;
}

static void GX_FreeHWSurface(_THIS, SDL_Surface *surface) {
    (void)_this;
    (void)surface;
}

static int GX_SetColors(_THIS, int first, int count, SDL_Color *colors) {
    (void)_this;
    (void)first;
    (void)count;
    (void)colors;
    /* The native surface is truecolour. SDL handles shadow palettes. */
    return 1;
}

static void GX_VideoQuit(_THIS) {
    struct SDL_PrivateVideoData *p = _this->hidden;
    struct gx_handle handle;
    if (p->fd >= 0) {
        if (p->window.window) {
            handle.version = GX_VERSION;
            handle.handle = p->window.window;
            ioctl(p->fd, GX_DESTROY, &handle);
        }
        close(p->fd);
        p->fd = -1;
    }
    if (_this->screen && _this->screen->pixels == p->buffer)
        _this->screen->pixels = NULL;
    SDL_free(p->buffer);
    SDL_free(p->converted);
    p->buffer = NULL;
    p->converted = NULL;
    p->window.window = 0;
    if (gx_device == _this)
        gx_device = NULL;
}

static void GX_DeleteDevice(_THIS) {
    GX_VideoQuit(_this);
    SDL_free(_this->hidden);
    SDL_free(_this);
}

static SDL_VideoDevice *GX_CreateDevice(int index) {
    SDL_VideoDevice *device;
    (void)index;
    device = SDL_malloc(sizeof(*device));
    if (!device) {
        SDL_OutOfMemory();
        return NULL;
    }
    SDL_memset(device, 0, sizeof(*device));
    device->hidden = SDL_malloc(sizeof(*device->hidden));
    if (!device->hidden) {
        SDL_free(device);
        SDL_OutOfMemory();
        return NULL;
    }
    SDL_memset(device->hidden, 0, sizeof(*device->hidden));
    device->hidden->fd = -1;
    device->VideoInit = GX_VideoInit;
    device->ListModes = GX_ListModes;
    device->SetVideoMode = GX_SetVideoMode;
    device->SetColors = GX_SetColors;
    device->UpdateRects = GX_UpdateRects;
    device->VideoQuit = GX_VideoQuit;
    device->AllocHWSurface = GX_AllocHWSurface;
    device->FreeHWSurface = GX_FreeHWSurface;
    device->SetCaption = GX_SetCaption;
    device->InitOSKeymap = GX_InitOSKeymap;
    device->PumpEvents = GX_PumpEvents;
    device->free = GX_DeleteDevice;
    return device;
}

VideoBootStrap GX_bootstrap = {"gx", "GX portable software video", GX_Available, GX_CreateDevice};
