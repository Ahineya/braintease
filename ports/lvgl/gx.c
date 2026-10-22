/* SPDX-License-Identifier: MIT */
#include "ui.h"
#include "ui-data.h"
#include "font-data.h"
#include <gx.h>
#include <gx-application.h>
#include <gx-panel.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "src/draw/lv_draw_private.h"
#include "src/draw/sw/blend/lv_draw_sw_blend_private.h"
#include "src/misc/lv_area_private.h"

#define MAX_WIDTH GX_HOSTED_MAX_WIDTH
#define MAX_HEIGHT GX_HOSTED_MAX_HEIGHT
/* A fixed strip budget: accepting wider windows must not multiply RAM use. */
#define DRAW_WIDTH 1024
#define DRAW_ROWS 256
static int fd = -1, failed, finished;
static struct gx_session_record application;
static int application_set;
static unsigned shift, control;
static int keyboard_group_changed;
static int keyboard_release_pending;
static unsigned minimum_width = 280, minimum_height = 360;
static struct gx_window window;
static lv_display_t *display;
static lv_indev_t *pointer, *keyboard;
static lv_group_t *group;
static lv_indev_data_t pointer_state, keyboard_state;
static uint8_t *draw;
static uint16_t *pixels;
static void (*on_text)(uint32_t);
static void (*on_action)(unsigned);
static void (*on_resize)(unsigned, unsigned);
static int (*on_close_request)(void);
static int (*on_key)(unsigned, int, unsigned);
static void (*on_idle)(void);
static int (*on_event)(const struct gx_event *);
/* GX reports physical edges. Repeat belongs to the toolkit, as in gterm/SDL,
 * and must also reach app filters which consume keys before LVGL sees them. */
static struct gx_event repeat_event;
static unsigned repeat_code;
static uint32_t repeat_at;
static int repeating;
static lv_group_t *repeat_group;
static lv_obj_t *repeat_focus;
static uint32_t panel_authority;
static int panel_open, panel_alpha;
static lv_obj_t *wheel_target;
static int32_t wheel_x, wheel_y;
static int profiling, direct_upload;
static int backing_cap, raster_cap, a8_cap;
static uint32_t draw_cells, draw_pixels;
static unsigned long profile_rects, profile_pixels, profile_frames;
static unsigned long profile_convert, profile_upload, profile_present;
static unsigned long profile_blends, profile_masks;
/* A renderer-owned colour strip avoids repeated C16 RGB reads and writes.
 * Queued small masks are owned here; large masks are submitted synchronously
 * while LVGL's source is still alive. Unsupported draws restore the current
 * strip before returning to the ordinary software path. */
#define RENDER_COMMANDS 256
#define RENDER_MASK_CELLS (128UL * 1024UL)
static int target_cap, target_state;
static uint32_t target_handle;
static unsigned target_width, target_height;
static lv_area_t target_area;
static uint32_t *target_commands;
static uint8_t *target_masks;
static unsigned target_count;
static uint32_t target_mask_used;
static unsigned long profile_target_batches, profile_target_commands;
static unsigned long profile_target_fallbacks, profile_target_readbacks, profile_rgb_uploads;
#define RENDER_MASK_TARGETS 8
struct target_mask {
    uint8_t *data;
    uint32_t address, span, handle, stride;
    unsigned width, height;
};
static struct target_mask target_mask_cache[RENDER_MASK_TARGETS];
static uint32_t *target_primitives;
static unsigned long target_primitive_capacity;
static unsigned long profile_target_mask_hits;
static uint32_t microseconds(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t))
        return 0;
    return (uint32_t)t.tv_sec * 1000000UL + t.tv_nsec / 1000UL;
}

void gx_lv_application(const char *app_id, const char *name, const char *icon) {
    int saved_errno = errno;
    application_set = gx_application_describe(&application, app_id, name, icon) == 0;
    errno = saved_errno;
}

void gx_lv_minimum(unsigned width, unsigned height) {
    if (width && width <= MAX_WIDTH)
        minimum_width = width;
    if (height && height <= MAX_HEIGHT)
        minimum_height = height;
}
void gx_lv_shortcuts(int (*key)(unsigned, int, unsigned)) { on_key = key; }
void gx_lv_finish(void) { finished = 1; }
void gx_lv_close_request(int (*request)(void)) { on_close_request = request; }
static void log_message(lv_log_level_t level, const char *message) {
    (void)level;
    fputs(message, stderr);
}

static uint32_t ticks(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t))
        return 0;
    return (uint32_t)t.tv_sec * 1000UL + t.tv_nsec / 1000000UL;
}

static void target_request(struct gx_render *r, unsigned operation) {
    memset(r, 0, sizeof(*r));
    r->version = GX_VERSION;
    r->operation = operation;
    r->window = window.window;
    r->target = target_handle;
}

static int target_submit(const uint8_t *data, uint32_t length) {
    struct gx_render r;
    unsigned count = target_count;
    if (!count)
        return 1;
    target_request(&r, GX_RENDER_BATCH);
    r.count = count;
    r.commands = target_commands;
    r.data = (uint8_t *)data;
    r.data_length = length;
    target_count = 0;
    target_mask_used = 0;
    if (ioctl(fd, GX_RENDER, &r) < 0) {
        failed = 1;
        return 0;
    }
    profile_target_batches++;
    profile_target_commands += count;
    return 1;
}

static int target_pending(void) {
    return target_submit(target_mask_used ? target_masks : NULL, target_mask_used);
}

/* This C16 transport uses banked pointers. Compare source ranges in logical
 * cells, including masks whose storage crosses one or more 64000-cell banks. */
static uint32_t target_address(const void *data) {
    uint32_t encoded = (uintptr_t)data;
    return (encoded >> 16) * 64000UL + (encoded & 65535UL);
}

static void target_mask_destroy(struct target_mask *mask) {
    struct gx_render r;
    if (mask->handle) {
        target_pending();
        target_request(&r, GX_RENDER_DESTROY);
        r.target = mask->handle;
        /* A rejected optional cache allocation/raster path must still leave
         * the ordinary guest-RAM renderer available. Window teardown also
         * releases any target whose destruction was rejected. */
        ioctl(fd, GX_RENDER, &r);
    }
    memset(mask, 0, sizeof(*mask));
}

void gx_lv_release_mask(void *base) {
    unsigned i;
    if (!base)
        return;
    for (i = 0; i < RENDER_MASK_TARGETS; i++)
        if (target_mask_cache[i].data == base)
            target_mask_destroy(&target_mask_cache[i]);
}

static struct target_mask *target_mask_find(const uint8_t *data, uint32_t stride,
                                            uint32_t width, uint32_t height,
                                            uint32_t *x, uint32_t *y) {
    uint32_t address = target_address(data), offset;
    unsigned i;
    for (i = 0; i < RENDER_MASK_TARGETS; i++) {
        struct target_mask *mask = &target_mask_cache[i];
        if (!mask->handle || stride != mask->stride || address < mask->address ||
            address - mask->address >= mask->span)
            continue;
        offset = address - mask->address;
        *x = offset % stride;
        *y = offset / stride;
        if (*x + width <= mask->width && *y + height <= mask->height)
            return mask;
    }
    return NULL;
}

static int target_mask_raster(uint8_t *data, unsigned width, unsigned height, uint32_t stride,
                               const struct gx_a8_primitive *primitives, unsigned long count) {
    struct target_mask *mask = NULL;
    struct gx_render r;
    uint32_t *words;
    unsigned i;
    unsigned long n;
    for (i = 0; i < RENDER_MASK_TARGETS; i++)
        if (target_mask_cache[i].data == data) {
            mask = &target_mask_cache[i];
            break;
        }
    if (mask && (mask->width != width || mask->height != height || mask->stride != stride))
        target_mask_destroy(mask);
    if (!mask)
        for (i = 0; i < RENDER_MASK_TARGETS; i++)
            if (!target_mask_cache[i].handle) {
                mask = &target_mask_cache[i];
                break;
            }
    if (!mask)
        return 0;
    if (count > target_primitive_capacity) {
        words = malloc(count * 6UL * sizeof(*words));
        if (!words) {
            target_mask_destroy(mask);
            return 0;
        }
        free(target_primitives);
        target_primitives = words;
        target_primitive_capacity = count;
    }
    if (!target_pending()) {
        target_mask_destroy(mask);
        return 0;
    }
    if (!mask->handle) {
        target_request(&r, GX_RENDER_CREATE);
        r.target = 0;
        r.width = width;
        r.height = height;
        r.format = GX_RENDER_A8;
        if (ioctl(fd, GX_RENDER, &r) < 0)
            return 0;
        mask->handle = r.target;
    }
    for (n = 0; n < count; n++) {
        words = target_primitives + n * 6;
        words[0] = primitives[n].x0;
        words[1] = primitives[n].y0;
        words[2] = primitives[n].x1;
        words[3] = primitives[n].y1;
        words[4] = primitives[n].dot;
        words[5] = primitives[n].size;
    }
    target_request(&r, GX_RENDER_RASTER_A8);
    r.target = mask->handle;
    r.flags = GX_A8_CLEAR;
    r.count = count;
    r.commands = count ? target_primitives : NULL;
    if (ioctl(fd, GX_RENDER, &r) < 0) {
        target_mask_destroy(mask);
        return 0;
    }
    target_request(&r, GX_RENDER_READBACK);
    r.target = mask->handle;
    r.width = width;
    r.height = height;
    r.stride = stride;
    r.data = data;
    r.data_length = (uint32_t)(height - 1) * stride + width;
    if (ioctl(fd, GX_RENDER, &r) < 0) {
        target_mask_destroy(mask);
        return 0;
    }
    mask->data = data;
    mask->address = target_address(data);
    mask->width = width;
    mask->height = height;
    mask->stride = stride;
    mask->span = r.data_length;
    return 1;
}

static void target_destroy(void) {
    struct gx_render r;
    if (target_handle) {
        target_request(&r, GX_RENDER_DESTROY);
        if (ioctl(fd, GX_RENDER, &r) < 0)
            failed = 1;
    }
    target_handle = 0;
    target_width = target_height = 0;
    target_count = 0;
    target_mask_used = 0;
    target_state = 0;
}

static uint32_t *target_command(void) {
    uint32_t *command;
    if (target_count == RENDER_COMMANDS && !target_pending())
        return NULL;
    command = target_commands + (uint32_t)target_count++ * 12;
    memset(command, 0, 12 * sizeof(*command));
    return command;
}

static int target_begin(const lv_area_t *area) {
    struct gx_render r;
    uint32_t *command;
    unsigned width = area->x2 - area->x1 + 1, height = area->y2 - area->y1 + 1;
    if (target_state) {
        if (memcmp(&target_area, area, sizeof(*area))) {
            /* The synchronous partial renderer must flush before reusing its
             * only colour buffer for another strip. Never discard queued work. */
            failed = 1;
            return 0;
        }
        return target_state == 1;
    }
    if (target_handle && (target_width != width || target_height != height))
        target_destroy();
    if (!target_handle) {
        target_request(&r, GX_RENDER_CREATE);
        r.width = width;
        r.height = height;
        r.format = GX_XRGB8888;
        if (ioctl(fd, GX_RENDER, &r) < 0) {
            failed = 1;
            return 0;
        }
        target_handle = r.target;
        target_width = width;
        target_height = height;
    }
    target_area = *area;
    target_state = 1;
    /* LVGL redraws the background of an invalidated strip. Clearing the native
     * storage also makes untouched pixels deterministic on its first use. */
    command = target_command();
    if (!command)
        return 0;
    command[0] = GX_RENDER_FILL;
    command[3] = width;
    command[4] = height;
    command[6] = 255;
    return 1;
}

static int target_software(lv_layer_t *layer) {
    struct gx_render r;
    if (target_state != 1)
        return 0;
    if (!target_pending())
        return 1;
    target_request(&r, GX_RENDER_READBACK);
    r.width = target_width;
    r.height = target_height;
    r.stride = layer->draw_buf->header.stride;
    r.data = draw;
    r.data_length = (uint32_t)(target_height - 1) * r.stride + (uint32_t)target_width * 3;
    if (ioctl(fd, GX_RENDER, &r) < 0) {
        failed = 1;
        return 1;
    }
    target_state = 2;
    profile_target_fallbacks++;
    profile_target_readbacks++;
    return 0;
}

/* This hook precedes RGB pointer formation in the adapted LVGL blend entry.
 * Only the primary colour strip is retained: intermediate layers remain
 * ordinary LVGL memory and are composited through a synchronized fallback. */
int gx_lv_render_blend(lv_draw_task_t *task, const lv_draw_sw_blend_dsc_t *dsc) {
    lv_layer_t *layer = task->target_layer;
    lv_area_t area;
    const uint8_t *mask = NULL;
    uint32_t stride = 0, width, height, cells, offset, row;
    uint32_t *command;
    struct target_mask *resident = NULL;
    uint32_t source_x = 0, source_y = 0;
    int direct = 0;
    if (!target_cap || !layer || !layer->draw_buf || layer->draw_buf->data != draw ||
        layer->color_format != LV_COLOR_FORMAT_RGB888)
        return 0;
    if (!lv_area_intersect(&area, dsc->blend_area, &task->clip_area))
        return 1;
    if (!target_begin(&layer->buf_area))
        return failed ? 1 : 0;
    if (dsc->src_buf || dsc->blend_mode != LV_BLEND_MODE_NORMAL)
        return target_software(layer);
    if (dsc->mask_buf && dsc->mask_res != LV_DRAW_SW_MASK_RES_FULL_COVER) {
        if (!dsc->mask_area)
            return target_software(layer);
        stride = dsc->mask_stride ? dsc->mask_stride : lv_area_get_width(dsc->mask_area);
        if (!stride || !lv_area_intersect(&area, &area, dsc->mask_area))
            return 1;
        mask = dsc->mask_buf + stride * (area.y1 - dsc->mask_area->y1) +
               (area.x1 - dsc->mask_area->x1);
    }
    width = lv_area_get_width(&area);
    height = lv_area_get_height(&area);
    cells = width * height;
    offset = 0;
    if (mask) {
        resident = target_mask_find(mask, stride, width, height, &source_x, &source_y);
        if (resident) {
            /* No guest pixels cross this request: coverage stays in the
             * renderer across strips, selection repaints and partial damage. */
            profile_target_mask_hits++;
        } else if (cells > RENDER_MASK_CELLS) {
            /* The graphics call snapshots this borrowed mask synchronously;
             * never retain a pointer into LVGL's temporary glyph/mask storage. */
            if (!target_pending())
                return 1;
            direct = 1;
        } else {
            if (target_mask_used + cells > RENDER_MASK_CELLS || target_count == RENDER_COMMANDS)
                if (!target_pending())
                    return 1;
            offset = target_mask_used;
            for (row = 0; row < height; row++)
                memcpy(target_masks + offset + row * width, mask + row * stride, width);
            target_mask_used += cells;
            stride = width;
        }
    }
    command = target_command();
    if (!command)
        return 1;
    command[0] = resident ? GX_RENDER_A8_TARGET : mask ? GX_RENDER_A8_DATA : GX_RENDER_FILL;
    command[1] = area.x1 - layer->buf_area.x1;
    command[2] = area.y1 - layer->buf_area.y1;
    command[3] = width;
    command[4] = height;
    command[5] = ((uint32_t)dsc->color.red << 16) | ((uint32_t)dsc->color.green << 8) |
                 dsc->color.blue;
    command[6] = dsc->opa;
    command[7] = resident ? resident->handle : offset;
    command[8] = resident ? source_x : stride;
    command[9] = resident ? source_y : 0;
    if (direct)
        target_submit(mask, (height - 1) * stride + width);
    return 1;
}

static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *map) {
    struct gx_upload u;
    struct gx_handle h;
    unsigned x, y, w = (unsigned)(a->x2 - a->x1 + 1), height = (unsigned)(a->y2 - a->y1 + 1);
    uint32_t stride = lv_display_get_buf_active(d)->header.stride;
    uint32_t at = profiling ? microseconds() : 0;
    if (a->x1 < 0 || a->y1 < 0 || a->x2 >= gx_lv_px(window.width) ||
        a->y2 >= gx_lv_px(window.height) || (uint32_t)w * height > draw_pixels ||
        (uint32_t)(height - 1) * stride + (uint32_t)w * (panel_alpha ? 4 : 3) > draw_cells) {
        failed = 1;
        lv_display_flush_ready(d);
        return;
    }
    if (panel_alpha) {
        struct gx_panel_upload p;
        memset(&p, 0, sizeof(p));
        p.version = GXP_VERSION;
        p.authority = panel_authority;
        p.window = window.window;
        p.x = a->x1;
        p.y = a->y1;
        p.width = w;
        p.height = height;
        p.stride = stride;
        p.pixels = map;
        if (ioctl(fd, GX_PANEL_UPLOAD, &p) < 0)
            failed = 1;
        if (profiling) {
            profile_rects++;
            profile_pixels += (uint32_t)w * height;
            profile_upload += microseconds() - at;
            at = microseconds();
        }
        goto present;
    }
    if (target_state == 1) {
        struct gx_render r;
        if (memcmp(a, &target_area, sizeof(*a)) || !target_pending()) {
            failed = 1;
            target_state = 0;
            lv_display_flush_ready(d);
            return;
        }
        target_request(&r, GX_RENDER_BLIT);
        r.width = w;
        r.height = height;
        r.dx = a->x1;
        r.dy = a->y1;
        if (ioctl(fd, GX_RENDER, &r) < 0)
            failed = 1;
        if (profiling) {
            profile_rects++;
            profile_pixels += (uint32_t)w * height;
            profile_upload += microseconds() - at;
            at = microseconds();
        }
        target_state = 0;
        goto present;
    }
    target_state = 0;
    memset(&u, 0, sizeof(u));
    u.version = GX_VERSION;
    u.surface = window.surface;
    u.x = a->x1;
    u.y = a->y1;
    u.width = w;
    u.height = height;
    u.stride = direct_upload ? stride : w;
    u.pixels = direct_upload ? (const uint16_t *)map : pixels;
    /* RGB888 is B,G,R, one logical channel per C character. Never reinterpret
     * it as packed words: C16 characters occupy sixteen physical bits. */
    if (!direct_upload)
        for (y = 0; y < height; y++) {
            uint8_t *p = map + (uint32_t)y * stride;
            for (x = 0; x < w; x++, p += 3)
                pixels[(uint32_t)y * w + x] =
                    ((p[2] & 248U) << 8) | ((p[1] & 252U) << 3) | (p[0] >> 3);
        }
    if (profiling) {
        if (!direct_upload)
            profile_convert += microseconds() - at;
        at = microseconds();
        profile_rects++;
        profile_pixels += (uint32_t)w * height;
    }
    if (ioctl(fd, direct_upload ? GX_UPLOAD_BGR888 : GX_UPLOAD, &u) < 0)
        failed = 1;
    profile_rgb_uploads++;
    if (profiling) {
        profile_upload += microseconds() - at;
        at = microseconds();
    }
present:
    if (lv_display_flush_is_last(d)) {
        h.version = GX_VERSION;
        h.handle = window.window;
        if (ioctl(fd, GX_PRESENT, &h) < 0)
            failed = 1;
        if (profiling) {
            profile_present += microseconds() - at;
            profile_frames++;
        }
    }
    lv_display_flush_ready(d);
}

static void pointer_read(lv_indev_t *i, lv_indev_data_t *data) {
    (void)i;
    *data = pointer_state;
}
static void keyboard_read(lv_indev_t *i, lv_indev_data_t *data) {
    (void)i;
    *data = keyboard_state;
}
/* Optional generic pixel primitive; LVGL owns layout, glyph data and colours. */
int gx_lv_blend(void *dst, long w, long h, long stride, const void *mask, long mask_stride,
                unsigned blue, unsigned green, unsigned red, unsigned opacity) {
    struct gx_raster_bgr888 r;
    if (!raster_cap)
        return 0;
    if (w <= 0 || h <= 0 || stride < 0 || stride > 65535UL || mask_stride < 0 ||
        mask_stride > 65535UL)
        return 0;
    if (profiling) {
        profile_blends++;
        if (mask)
            profile_masks++;
    }
    memset(&r, 0, sizeof(r));
    r.version = GX_VERSION;
    r.dst = dst;
    r.mask = mask;
    r.width = w;
    r.height = h;
    r.dst_stride = stride;
    r.mask_stride = mask_stride;
    r.blue = blue;
    r.green = green;
    r.red = red;
    r.opacity = opacity;
    if (ioctl(fd, GX_RASTER_BGR888, &r) < 0) {
        failed = 1;
        return 0;
    }
    return 1;
}
int gx_lv_raster_a8(uint8_t *dst, long w, long h, long stride,
                    const struct gx_a8_primitive *primitives, unsigned long count) {
    struct gx_raster_a8 r;
    if (!a8_cap && !target_cap)
        return 0;
    if (!dst || w <= 0 || h <= 0 || w > MAX_WIDTH * 2L || h > MAX_HEIGHT * 2L ||
        stride < w || stride > 65535UL || count > 8192UL || (count && !primitives))
        return 0;
    if (target_cap && target_mask_raster(dst, w, h, stride, primitives, count))
        return 1;
    /* A failed optional resident cache cannot make a later blend reuse stale
     * coverage after the fallback has changed its corresponding RAM mask. */
    gx_lv_release_mask(dst);
    if (!a8_cap)
        return 0;
    memset(&r, 0, sizeof(r));
    r.version = GX_VERSION;
    r.dst = dst;
    r.width = w;
    r.height = h;
    r.stride = stride;
    r.flags = GX_A8_CLEAR;
    r.count = count;
    r.primitives = primitives;
    if (ioctl(fd, GX_RASTER_A8, &r) < 0) {
        failed = 1;
        return 0;
    }
    return 1;
}
static unsigned backing(void) {
    struct gx_backing b;
    const char *override = getenv("LVGL_SCALE");
    if (!backing_cap)
        return 1;
    memset(&b, 0, sizeof(b));
    b.version = GX_VERSION;
    b.window = window.window;
    b.scale = override && !strcmp(override, "1") ? 1 : override && !strcmp(override, "2") ? 2 : 0;
    if (ioctl(fd, GX_BACKING_SCALE, &b) < 0) {
        failed = 1;
        return 1;
    }
    return b.scale;
}
static int buffers(unsigned scale) {
    /* Keep the existing strip memory budget, independent of window width. */
    uint32_t count = (uint32_t)DRAW_WIDTH * scale * scale * DRAW_ROWS;
    unsigned channels = panel_alpha ? 4 : 3;
    uint8_t *next = malloc(count * channels);
    uint16_t *converted = NULL;
    if (!direct_upload && !panel_alpha)
        converted = malloc(count * sizeof(*converted));
    if (!next || (!direct_upload && !panel_alpha && !converted)) {
        free(next);
        free(converted);
        return -1;
    }
    free(draw);
    free(pixels);
    draw = next;
    pixels = converted;
    draw_pixels = count;
    draw_cells = count * channels;
    if (display)
        lv_display_set_buffers(display, draw, NULL, draw_cells, LV_DISPLAY_RENDER_MODE_PARTIAL);
    return 0;
}

static int open_window(const char *title, unsigned width, unsigned height) {
    struct gx_configure c;
    struct gx_info info;
    struct gx_formats formats;
    struct gx_raster_a8 a8;
    struct gx_render render;
    profiling = getenv("LVGL_PROFILE") != NULL;
    failed = finished = 0;
    target_cap = target_state = 0;
    profile_rects = profile_pixels = profile_frames = 0;
    profile_convert = profile_upload = profile_present = 0;
    profile_blends = profile_masks = 0;
    profile_target_batches = profile_target_commands = profile_target_fallbacks = 0;
    profile_target_readbacks = profile_rgb_uploads = 0;
    profile_target_mask_hits = 0;
    if (width > MAX_WIDTH || height > MAX_HEIGHT)
        return -1;
    fd = open("/dev/gfx", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("/dev/gfx");
        return -1;
    }
    if (application_set) {
        int saved_errno = errno;
        (void)gx_application_register(fd, &application);
        errno = saved_errno;
    }
    memset(&info, 0, sizeof(info));
    info.version = GX_VERSION;
    direct_upload = ioctl(fd, GX_INFO, &info) >= 0 && (info.flags & GX_CAP_BGR888_UPLOAD) &&
                    !getenv("LVGL_SOFTWARE_UPLOAD");
    backing_cap = info.flags & GX_CAP_BACKING_SCALE;
    raster_cap = (info.flags & GX_CAP_BGR888_RASTER) && !getenv("LVGL_SOFTWARE_RASTER");
    a8_cap = 0;
    if (!getenv("LVGL_SOFTWARE_MASK")) {
        memset(&a8, 0, sizeof(a8));
        a8.version = GX_VERSION;
        a8_cap = ioctl(fd, GX_RASTER_A8, &a8) >= 0;
    }
    memset(&window, 0, sizeof(window));
    window.version = GX_VERSION;
    window.width = width;
    window.height = height;
    window.format = GX_RGB565;
    memset(&formats, 0, sizeof(formats));
    formats.version = GX_VERSION;
    if ((panel_alpha || (direct_upload && !getenv("LVGL_RGB565"))) && ioctl(fd, GX_FORMATS, &formats) == 0 &&
        (formats.formats & GX_FORMAT_XRGB8888))
        window.format = GX_XRGB8888;
    if (panel_alpha && window.format != GX_XRGB8888) {
        gx_lv_close();
        return -1;
    }
    /* Existing diagnostic switches keep exercising the complete original
     * transport, including RGB565 conversion and the portable blend loops. */
    if (!panel_alpha && window.format == GX_XRGB8888 && !getenv("LVGL_SOFTWARE_TARGET") &&
        !getenv("LVGL_SOFTWARE_UPLOAD") && !getenv("LVGL_SOFTWARE_RASTER") &&
        !getenv("LVGL_SOFTWARE_MASK")) {
        memset(&render, 0, sizeof(render));
        render.version = GX_VERSION;
        if (ioctl(fd, GX_RENDER, &render) >= 0) {
            target_commands = malloc(RENDER_COMMANDS * 12UL * sizeof(*target_commands));
            target_masks = malloc(RENDER_MASK_CELLS);
            if (target_commands && target_masks)
                target_cap = 1;
            else {
                free(target_commands);
                free(target_masks);
                target_commands = NULL;
                target_masks = NULL;
            }
        }
    }
    strncpy(window.title, title, sizeof(window.title) - 1);
    if (ioctl(fd, panel_open ? GX_CREATE_HIDDEN : GX_CREATE, &window) < 0) {
        perror("GX_CREATE");
        gx_lv_close();
        return -1;
    }
    if (panel_open) {
        struct gx_panel p;
        memset(&p, 0, sizeof(p));
        p.version = GXP_VERSION;
        p.operation = GXP_CONFIGURE;
        p.authority = panel_authority;
        p.window = window.window;
        p.flags = panel_alpha ? GXP_ALPHA : 0;
        if (ioctl(fd, GX_PANEL, &p) < 0) {
            gx_lv_close();
            return -1;
        }
    }
    memset(&c, 0, sizeof(c));
    c.version = GX_VERSION;
    c.window = window.window;
    c.width = width;
    c.height = height;
    if (ioctl(fd, GX_CONFIGURE, &c) < 0) {
        gx_lv_close();
        return -1;
    }
    if (gx_font_data_open() < 0) {
        fputs("LVGL: cannot allocate font data\n", stderr);
        gx_lv_close();
        return -1;
    }
    lv_init();
    lv_log_register_print_cb(log_message);
    lv_tick_set_cb(ticks);
    {
        unsigned scale = backing();
        if (failed || buffers(scale) < 0) {
            gx_lv_close();
            return -1;
        }
        display = lv_display_create(width * scale, height * scale);
        if (!display) {
            gx_lv_close();
            return -1;
        }
        lv_display_set_color_format(display, panel_alpha ? LV_COLOR_FORMAT_ARGB8888 : LV_COLOR_FORMAT_RGB888);
        lv_display_set_buffers(display, draw, NULL, draw_cells, LV_DISPLAY_RENDER_MODE_PARTIAL);
        lv_display_set_flush_cb(display, flush);
        gx_lv_apply_density(scale);
        /* Installing the shared theme styles LVGL's bottom layer as a canvas.
         * A transparent active screen alone would still flatten every panel
         * against that canvas before upload. Keep the display backing clear. */
        if (panel_alpha)
            lv_obj_set_style_bg_opa(lv_layer_bottom(), LV_OPA_TRANSP, 0);
    }
    group = lv_group_create();
    lv_group_set_default(group);
    pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, pointer_read);
    lv_indev_set_mode(pointer, LV_INDEV_MODE_EVENT);
    ui_attach_pointer(pointer);
    keyboard = lv_indev_create();
    lv_indev_set_type(keyboard, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(keyboard, keyboard_read);
    lv_indev_set_mode(keyboard, LV_INDEV_MODE_EVENT);
    lv_indev_set_group(keyboard, group);
    return 0;
}

int gx_lv_open(const char *title, unsigned width, unsigned height) {
    panel_open = panel_alpha = 0;
    return open_window(title, width, height);
}
int gx_lv_open_panel(const char *title, uint32_t authority, unsigned *width, unsigned *height) {
    struct gx_panel p;
    int probe = open("/dev/gfx", O_RDWR | O_CLOEXEC);
    if (probe < 0)
        return -1;
    memset(&p, 0, sizeof(p));
    p.version = GXP_VERSION;
    p.operation = GXP_INFO;
    if (ioctl(probe, GX_PANEL, &p) < 0 || !(p.flags & GXP_CAP_OVERLAY)) {
        close(probe);
        return -1;
    }
    close(probe);
    panel_open = 1;
    panel_alpha = (p.flags & GXP_CAP_ALPHA) != 0;
    panel_authority = authority;
    minimum_width = minimum_height = 1;
    *width = p.width;
    *height = p.height;
    return open_window(title, *width, *height);
}
int gx_lv_fd(void) { return fd; }
int gx_lv_panel_alpha(void) { return panel_alpha; }
uint32_t gx_lv_window(void) { return window.window; }
void gx_lv_idle(void (*poll)(void)) { on_idle = poll; }
void gx_lv_events(int (*filter)(const struct gx_event *)) { on_event = filter; }

void gx_lv_handlers(void (*text)(uint32_t), void (*action)(unsigned),
                    void (*resize)(unsigned, unsigned)) {
    on_text = text;
    on_action = action;
    on_resize = resize;
}
lv_group_t *gx_lv_group(void) { return group; }
void gx_lv_cancel_key_repeat(void) {
    repeat_code = 0;
    repeat_group = NULL;
    repeat_focus = NULL;
    if (keyboard) {
        keyboard_state.state = LV_INDEV_STATE_RELEASED;
        lv_indev_reset(keyboard, NULL);
        lv_indev_wait_release(keyboard);
        keyboard_release_pending = 1;
    }
}
static void settle_keyboard(void) {
    /* Context replacement can happen inside an LVGL callback. Finish the
     * silent release after that callback, without nesting input processing
     * or turning a canceled Enter press into a button click. */
    if (keyboard && keyboard_release_pending) {
        keyboard_release_pending = 0;
        keyboard_state.state = LV_INDEV_STATE_RELEASED;
        lv_indev_read(keyboard);
    }
}
static void reset_wheel(void) {
    wheel_target = NULL;
    wheel_x = wheel_y = 0;
}

void gx_lv_set_group(lv_group_t *next) {
    gx_lv_cancel_key_repeat();
    reset_wheel();
    group = next;
    lv_group_set_default(next);
    lv_indev_set_group(keyboard, next);
    lv_indev_reset(keyboard, NULL);
    keyboard_group_changed = 1;
}

int gx_lv_clipboard(char *text, unsigned capacity, int write) {
    struct gx_clipboard c;
    if (!text || !capacity)
        return -1;
    memset(&c, 0, sizeof(c));
    c.version = GX_VERSION;
    c.window = window.window;
    c.text = text;
    /* The wrapper's capacity includes a C terminator; GX counts payload cells. */
    c.capacity = write ? 0 : capacity - 1;
    if (c.capacity > GX_CLIPBOARD_MAX)
        c.capacity = GX_CLIPBOARD_MAX;
    c.length = write ? strlen(text) : 0;
    if (c.length > GX_CLIPBOARD_MAX || (write && c.length >= capacity))
        return -1;
    if (ioctl(fd, write ? GX_CLIPBOARD_WRITE : GX_CLIPBOARD_READ, &c) < 0)
        return -1;
    if (!write) {
        if (c.length >= capacity)
            return -1;
        text[c.length] = 0;
    }
    return 0;
}

static void release_inputs(void) {
    gx_lv_cancel_key_repeat();
    reset_wheel();
    lv_indev_reset(pointer, NULL);
    lv_indev_wait_release(pointer);
    lv_indev_reset(keyboard, NULL);
    lv_indev_wait_release(keyboard);
    pointer_state.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(pointer);
    keyboard_state.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(keyboard);
    keyboard_release_pending = 0;
    shift = control = 0;
    keyboard_group_changed = 0;
}

static uint32_t special(unsigned code) {
    switch (code) {
    case 40:
    case 88:
        return LV_KEY_ENTER;
    case 41:
        return LV_KEY_ESC;
    case 42:
        return LV_KEY_BACKSPACE;
    case 43:
        return shift ? LV_KEY_PREV : LV_KEY_NEXT;
    case 74:
        return LV_KEY_HOME;
    case 76:
        return LV_KEY_DEL;
    case 77:
        return LV_KEY_END;
    case 79:
        return LV_KEY_RIGHT;
    case 80:
        return LV_KEY_LEFT;
    case 81:
        return LV_KEY_DOWN;
    case 82:
        return LV_KEY_UP;
    default:
        return 0;
    }
}

static uint32_t printable(unsigned code) {
    const char *digits = "1234567890", *symbols = "!@#$%^&*()";
    if (code >= 4 && code <= 29)
        return (shift ? 'A' : 'a') + code - 4;
    if (code >= 30 && code <= 39)
        return (shift ? symbols : digits)[code - 30];
    if (code >= 89 && code <= 97)
        return '1' + code - 89;
    switch (code) {
    case 44:
        return ' ';
    case 45:
        return shift ? '_' : '-';
    case 46:
        return shift ? '+' : '=';
    case 47:
        return shift ? '{' : '[';
    case 48:
        return shift ? '}' : ']';
    case 49:
        return shift ? '|' : '\\';
    case 51:
        return shift ? ':' : ';';
    case 52:
        return shift ? '"' : '\'';
    case 53:
        return shift ? '~' : '`';
    case 54:
        return shift ? '<' : ',';
    case 55:
        return shift ? '>' : '.';
    case 56:
        return shift ? '?' : '/';
    case 84:
        return '/';
    case 85:
        return '*';
    case 86:
        return '-';
    case 87:
        return '+';
    case 98:
        return '0';
    case 99:
        return '.';
    default:
        return 0;
    }
}

static void wheel(struct gx_event *e) {
    lv_point_t p;
    lv_obj_t *target;
    int32_t dx = e->wheel_x, dy = e->wheel_y;
    if (e->x < 0 || e->y < 0 || e->x >= window.width || e->y >= window.height) {
        reset_wheel();
        return;
    }
    p.x = (int32_t)e->x * gx_lv_density();
    p.y = (int32_t)e->y * gx_lv_density();
    /* Match LVGL's pointer hit testing: menus and dialogs live above the
     * active screen. Stop at the first hit layer so an overlay backdrop
     * blocks scrolling of the content underneath it. */
    target = lv_indev_search_obj(lv_display_get_layer_sys(display), &p);
    if (!target)
        target = lv_indev_search_obj(lv_display_get_layer_top(display), &p);
    if (!target)
        target = lv_indev_search_obj(lv_display_get_screen_active(display), &p);
    if (!target)
        target = lv_indev_search_obj(lv_display_get_layer_bottom(display), &p);
    while (target && !lv_obj_is_scrollable(target))
        target = lv_obj_get_parent(target);
    if (!target) {
        reset_wheel();
        return;
    }
    if (target != wheel_target) {
        wheel_target = target;
        wheel_x = wheel_y = 0;
    }
    /* Retain sub-pixel trackpad motion. Bound input before multiplying. */
    if (dx > 6553600L)
        dx = 6553600L;
    if (dx < -6553600L)
        dx = -6553600L;
    if (dy > 6553600L)
        dy = 6553600L;
    if (dy < -6553600L)
        dy = -6553600L;
    wheel_x += dx * (40L * gx_lv_density());
    wheel_y += dy * (40L * gx_lv_density());
    dx = wheel_x / 65536L;
    dy = wheel_y / 65536L;
    wheel_x %= 65536L;
    wheel_y %= 65536L;
    if (!(lv_obj_get_scroll_dir(target) & LV_DIR_HOR))
        dx = 0;
    if (!(lv_obj_get_scroll_dir(target) & LV_DIR_VER))
        dy = 0;
    lv_obj_scroll_by_bounded(target, dx, dy, LV_ANIM_OFF);
}

static void dispatch_event(struct gx_event *e) {
    uint32_t k;
    settle_keyboard();
    if (on_event && on_event(e))
        return;
    if (e->type == GX_MOTION || e->type == GX_BUTTON) {
        pointer_state.point.x = e->x < 0               ? 0
                                : e->x >= window.width ? gx_lv_px(window.width) - 1
                                                       : (int32_t)e->x * gx_lv_density();
        pointer_state.point.y = e->y < 0                ? 0
                                : e->y >= window.height ? gx_lv_px(window.height) - 1
                                                        : (int32_t)e->y * gx_lv_density();
        if (pointer_state.point.x < 0)
            pointer_state.point.x = 0;
        if (pointer_state.point.y < 0)
            pointer_state.point.y = 0;
        if (pointer_state.point.x >= gx_lv_px(window.width))
            pointer_state.point.x = gx_lv_px(window.width) - 1;
        if (pointer_state.point.y >= gx_lv_px(window.height))
            pointer_state.point.y = gx_lv_px(window.height) - 1;
        pointer_state.state = (e->buttons & 1) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        lv_indev_read(pointer);
    } else if (e->type == GX_WHEEL)
        wheel(e);
    else if (e->type == GX_FOCUS && !(e->flags & GX_FOCUSED))
        release_inputs();
    else if (e->type == GX_OVERFLOW)
        release_inputs();
    else if (e->type == GX_TEXT) {
        if (!ui_overlay_text(e->dx) && on_text)
            on_text(e->dx);
    } else if (e->type == GX_ACTION) {
        if (!ui_overlay_action(e->code) && on_action)
            on_action(e->code);
    } else if (e->type == GX_KEY) {
        if (keyboard_group_changed) {
            keyboard_group_changed = 0;
            lv_indev_wait_release(keyboard);
            keyboard_state.state = LV_INDEV_STATE_RELEASED;
            lv_indev_read(keyboard);
        }
        if (e->code == 225 || e->code == 229) {
            unsigned bit = 1U << (e->code - 224);
            if (e->flags & GX_DOWN)
                shift |= bit;
            else
                shift &= ~bit;
        } else if (e->code == 224 || e->code == 228 || e->code == 227 || e->code == 231) {
            unsigned bit = 1U << (e->code - 224);
            if (e->flags & GX_DOWN)
                control |= bit;
            else
                control &= ~bit;
        } else if (ui_overlay_key(e->code, (e->flags & GX_DOWN) != 0,
                                  (control ? 1 : 0) | (shift ? 2 : 0)))
            return;
        else if (on_key && !ui_overlay_active() &&
                 on_key(e->code, (e->flags & GX_DOWN) != 0, (control ? 1 : 0) | (shift ? 2 : 0)))
            return;
        else if ((k = special(e->code)) != 0) {
            int begin_traversal = (e->flags & GX_DOWN) && (k == LV_KEY_NEXT || k == LV_KEY_PREV) &&
                                  !lv_group_get_focused(group);
            /* The event-mode keypad has no polling timer. Make each toolkit
             * repeat a fresh keypad press, without inventing app-visible ups.
             * Only navigation/edit keys can enter this path. */
            if (repeating) {
                keyboard_state.state = LV_INDEV_STATE_RELEASED;
                lv_indev_read(keyboard);
            }
            keyboard_state.key = k;
            keyboard_state.state =
                (e->flags & GX_DOWN) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
            lv_indev_read(keyboard);
            /* LVGL records the edge but returns early when no object is
             * focused. Start traversal once; release/repeat remain on its
             * normal keypad path, with no extra Tab step or activation. */
            if (begin_traversal && !lv_group_get_focused(group)) {
                if (k == LV_KEY_NEXT)
                    lv_group_focus_next(group);
                else
                    lv_group_focus_prev(group);
            }
        } else if ((e->flags & GX_DOWN) && !(e->flags & GX_TEXT_KEY) && !control &&
                   (k = printable(e->code))) {
            if (!ui_overlay_text(k) && on_text)
                on_text(k);
        }
    } else if (e->type == GX_RESIZE) {
        struct gx_configure c;
        unsigned w = e->x < minimum_width ? minimum_width : e->x > MAX_WIDTH ? MAX_WIDTH : e->x;
        unsigned h = e->y < minimum_height ? minimum_height : e->y > MAX_HEIGHT ? MAX_HEIGHT : e->y;
        release_inputs();
        memset(&c, 0, sizeof(c));
        c.version = GX_VERSION;
        c.window = window.window;
        c.width = w;
        c.height = h;
        if (ioctl(fd, GX_CONFIGURE, &c) < 0) {
            failed = 1;
            return;
        }
        window.width = w;
        window.height = h;
        if (pointer_state.point.x < 0)
            pointer_state.point.x = 0;
        if (pointer_state.point.y < 0)
            pointer_state.point.y = 0;
        {
            unsigned scale = backing();
            if (scale != gx_lv_density() && buffers(scale) < 0) {
                failed = 1;
                return;
            }
            gx_lv_apply_density(scale);
            if (pointer_state.point.x >= gx_lv_px(w))
                pointer_state.point.x = gx_lv_px(w) - 1;
            if (pointer_state.point.y >= gx_lv_px(h))
                pointer_state.point.y = gx_lv_px(h) - 1;
            lv_display_set_resolution(display, gx_lv_px(w), gx_lv_px(h));
        }
        if (on_resize)
            on_resize(w, h);
        ui_data_resize();
    }
}

static int repeatable(unsigned code) {
    return code == 42 || (code >= 74 && code <= 82);
}

static void event(struct gx_event *e) {
    int arm = 0;
    if (!repeating) {
        /* Cancel before a raw filter can consume the real release. */
        if ((e->type == GX_KEY && !(e->flags & GX_DOWN) && e->code == repeat_code) ||
            (e->type == GX_FOCUS && !(e->flags & GX_FOCUSED)) ||
            e->type == GX_OVERFLOW || e->type == GX_RESIZE || e->type == GX_ACTION ||
            e->type == GX_TEXT || (e->type == GX_BUTTON && e->buttons))
            gx_lv_cancel_key_repeat();
        if (e->type == GX_KEY && (e->flags & GX_DOWN) && e->code < 224) {
            gx_lv_cancel_key_repeat();
            if (repeatable(e->code) && !(e->flags & GX_TEXT_KEY)) {
                repeat_event = *e;
                repeat_code = e->code;
                repeat_at = lv_tick_get() + 500UL;
                arm = 1;
            }
        }
    }
    dispatch_event(e);
    /* The first press may establish focus. An explicit cancellation by its
     * handler (for example closing a menu) must still win. */
    if (arm && repeat_code == e->code) {
        repeat_group = group;
        repeat_focus = group ? lv_group_get_focused(group) : NULL;
    }
}

static void repeat_key(void) {
    struct gx_event e;
    if (!repeat_code) return;
    if (repeat_group != group || !repeat_focus || lv_group_get_focused(group) != repeat_focus) {
        gx_lv_cancel_key_repeat();
        return;
    }
    if ((int32_t)(lv_tick_get() - repeat_at) < 0) return;
    e = repeat_event;
    repeat_at = lv_tick_get() + 100UL;
    repeating = 1;
    event(&e);
    repeating = 0;
}

int gx_lv_run(void) {
    while (!failed && !finished) {
        struct gx_event e;
        uint32_t delay;
        unsigned count;
        if (on_idle)
            on_idle();
        /* Bound the batch so an input flood cannot starve painting/timers. */
        for (count = 0; count < 128; count++) {
            memset(&e, 0, sizeof(e));
            e.version = GX_VERSION;
            e.window = window.window;
            if (ioctl(fd, GX_EVENT, &e) < 0)
                return 1;
            if (!e.type)
                break;
            if (e.type == GX_CLOSE) {
                if (!on_close_request || on_close_request())
                    return 0;
                continue;
            }
            event(&e);
            if (finished)
                break;
        }
        if (finished || failed)
            break;
        /* Drain releases before repeating; a flooded queue must not replay a
         * held key after its release is already waiting in the next batch. */
        settle_keyboard();
        if (count < 128) repeat_key();
        delay = lv_timer_handler();
        if (finished || failed)
            break;
        if (count == 128)
            continue;
        if (delay > 1000UL)
            delay = 1000UL;
        if (repeat_code) {
            int32_t remaining = (int32_t)(repeat_at - lv_tick_get());
            if (remaining <= 0) delay = 0;
            else if ((uint32_t)remaining < delay) delay = remaining;
        }
        if (delay) {
            fd_set set;
            struct timeval t;
            t.tv_sec = delay / 1000UL;
            t.tv_usec = (delay % 1000UL) * 1000UL;
            FD_ZERO(&set);
            FD_SET(fd, &set);
            if (select(fd + 1, &set, NULL, NULL, &t) < 0 && errno != EINTR)
                return 1;
        }
    }
    if (failed)
        fputs("LVGL: graphics operation failed\n", stderr);
    return failed ? 1 : 0;
}

void gx_lv_close(void) {
    struct gx_handle h;
    unsigned i;
    gx_lv_cancel_key_repeat();
    reset_wheel();
    if (display) {
        ui_theme_close();
        lv_deinit();
        display = NULL;
        pointer = keyboard = NULL;
        group = NULL;
        keyboard_release_pending = 0;
    }
    gx_font_data_close();
    if (fd >= 0) {
        if (panel_open && window.window) {
            struct gx_panel p;
            memset(&p, 0, sizeof(p));
            p.version = GXP_VERSION;
            p.operation = GXP_RELEASE;
            p.authority = panel_authority;
            p.window = window.window;
            ioctl(fd, GX_PANEL, &p);
        }
        for (i = 0; i < RENDER_MASK_TARGETS; i++)
            target_mask_destroy(&target_mask_cache[i]);
        target_destroy();
        if (window.window) {
            h.version = GX_VERSION;
            h.handle = window.window;
            ioctl(fd, GX_DESTROY, &h);
        }
        close(fd);
        fd = -1;
    }
    free(draw);
    free(pixels);
    free(target_commands);
    free(target_masks);
    free(target_primitives);
    draw = NULL;
    pixels = NULL;
    target_commands = NULL;
    target_masks = NULL;
    target_primitives = NULL;
    target_primitive_capacity = 0;
    window.window = 0;
    a8_cap = 0;
    target_cap = 0;
    if (profiling)
        fprintf(stderr,
                "LVGL_PROFILE frames=%lu rects=%lu pixels=%lu convert_us=%lu upload_us=%lu "
                "present_us=%lu blends=%lu masks=%lu direct_upload=%d format=%u "
                "target_batches=%lu target_commands=%lu target_fallbacks=%lu "
                "target_readbacks=%lu rgb_uploads=%lu target_mask_hits=%lu\n",
                profile_frames, profile_rects, profile_pixels, profile_convert, profile_upload,
                profile_present, profile_blends, profile_masks, direct_upload, window.format,
                profile_target_batches, profile_target_commands, profile_target_fallbacks,
                profile_target_readbacks, profile_rgb_uploads, profile_target_mask_hits);
}
