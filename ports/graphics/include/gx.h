/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RIPPLE_GX_H
#define RIPPLE_GX_H
#include <stdint.h>
/* Guest ABI: C16 cells, opaque generation handles, never host pointers.
 * All buffers are borrowed only for the duration of the synchronous call. */
#define GX_VERSION 1
#define GX_RGB565 1
#define GX_CAP_COPY 1
#define GX_CAP_FILL 2
#define GX_CAP_INPUT 4
#define GX_CAP_MULTIWINDOW 8
#define GX_CAP_RESIZE 16
#define GX_CAP_TITLE 32
#define GX_INFO 0x4700
#define GX_CREATE 0x4701
#define GX_UPLOAD 0x4702
#define GX_PRESENT 0x4703
#define GX_FILL 0x4704
#define GX_COPY 0x4705
#define GX_EVENT 0x4706
#define GX_STATE 0x4707
#define GX_DESTROY 0x4708
#define GX_TITLE 0x4709
#define GX_WAIT 0x470a
#define GX_CONFIGURE 0x470b
#define GX_CREATE_HIDDEN 0x470c
#define GX_ACTIVATE 0x470d
#define GX_WINDOW_FIXED 1
#define GX_WINDOW_HIDDEN 2
#define GX_READY_INPUT 1
#define GX_READY_READ 2
#define GX_READY_WRITE 4
#define GX_KEY 1
#define GX_MOTION 2
#define GX_BUTTON 3
#define GX_WHEEL 4
#define GX_ENTER 5
#define GX_FOCUS 6
#define GX_CAPTURE 7
#define GX_CLOSE 8
#define GX_OVERFLOW 9
#define GX_RESIZE 10
#define GX_DOWN 1
#define GX_FOCUSED 8
struct gx_info {
    uint16_t version, flags, max_width, max_height, max_windows, format;
    uint32_t max_surface_cells;
};
struct gx_window {
    uint16_t version, width, height, format;
    uint32_t window, surface;
    char title[64];
};
struct gx_handle { uint16_t version; uint32_t handle; };
/* GX_TITLE shares the handle prefix; the title is a bounded C16 C string. */
struct gx_title { uint16_t version; uint32_t window; char title[64]; };
/* Configure client pixels and native resize/visibility policy. Host resize
 * requests arrive as GX_RESIZE with x/y = requested content width/height;
 * the owner acknowledges by configuring its accepted dimensions. */
struct gx_configure { uint16_t version; uint32_t window; uint16_t width, height, flags; };
/* Atomically wait for this window and an owned PTY master. Timeout is in
 * deciseconds (0 polls); ready returns the requested ready bits. */
struct gx_wait {
    uint16_t version;
    uint32_t window;
    uint16_t master, flags, timeout, ready;
    uint32_t milliseconds;
};
struct gx_upload {
    uint16_t version;
    uint32_t surface;
    uint16_t x, y, width, height, stride;
    const uint16_t *pixels;
};
struct gx_fill {
    uint16_t version;
    uint32_t surface;
    uint16_t x, y, width, height, colour;
};
struct gx_copy {
    uint16_t version;
    uint32_t surface;
    uint16_t sx, sy, dx, dy, width, height;
};
/* Input is nonblocking: type=0 when no event is available. Key codes are USB
 * HID usages; x/y are content pixels, wheel values are signed 16.16 lines. */
struct gx_event {
    uint16_t version;
    uint32_t window, sequence;
    uint16_t type, flags, code, buttons;
    int32_t x, y, dx, dy, wheel_x, wheel_y;
};
struct gx_state {
    uint16_t version;
    uint32_t window;
    uint16_t flags, buttons, held[16];
    int32_t x, y;
};
#endif
