/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RIPPLE_GX_H
#define RIPPLE_GX_H
#include <stdint.h>
/* Guest ABI: C16 cells, opaque generation handles, never host pointers.
 * All buffers are borrowed only for the duration of the synchronous call. */
#define GX_VERSION 1
/* Hosted surface bounds; the VM desktop advertises its own GX_INFO geometry. */
#define GX_HOSTED_MAX_WIDTH 1920
#define GX_HOSTED_MAX_HEIGHT 1080
#define GX_RGB565 1
#define GX_CAP_COPY 1
#define GX_CAP_FILL 2
#define GX_CAP_INPUT 4
#define GX_CAP_MULTIWINDOW 8
#define GX_CAP_RESIZE 16
#define GX_CAP_TITLE 32
#define GX_CAP_DESKTOP_POINTER 64
#define GX_CAP_RASTER 128
#define GX_CAP_SCALED_UPLOAD 256
#define GX_CAP_INDEXED_UPLOAD 512
#define GX_CAP_CLIPBOARD 1024
#define GX_CAP_TERMINAL 2048
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
#define GX_DESKTOP_POINTER 0x470e
#define GX_RASTER_FILL 0x470f
#define GX_RASTER_COPY 0x4710
/* gx_upload source geometry; x=y=0. Fit the source into the whole surface
 * with nearest-neighbour sampling and black borders, preserving its aspect. */
#define GX_UPLOAD_SCALED 0x4711
#define GX_UPLOAD_INDEXED 0x4712
/* Plain UTF-8 text, one octet per C16 cell, excluding the terminating NUL. */
#define GX_CLIPBOARD_READ 0x4713
#define GX_CLIPBOARD_WRITE 0x4714
#define GX_CLIPBOARD_MAX 32768UL
/* Optional hosted text renderer. Logical cell metrics preserve guest mouse/
 * resize coordinates; the host may rasterize at Retina resolution. A row is
 * an owned-on-call stream of colour, width (0/1/2), UTF-8 length, octet cells
 * for each column, including zero-length continuation cells. No VT internals
 * or process-local grapheme IDs cross the ABI. */
#define GX_TERMINAL_METRICS 0x4715
#define GX_TERMINAL_DRAW 0x4716
struct gx_terminal_metrics { uint16_t version, width, height; };
struct gx_terminal_draw {
    uint16_t version;uint32_t window;
    uint16_t cols,rows,row,cursor_x,cursor_y,cursor_shape,cursor_visible;
    const uint16_t *data;uint32_t length;
};
#define GX_ACTION 12
#define GX_ACTION_COPY 1
#define GX_ACTION_PASTE 2
#define GX_ACTION_CUT 3
#define GX_ACTION_SELECT_ALL 4
struct gx_clipboard {
    uint16_t version; uint32_t window;
    char *text; uint32_t length, capacity;
};
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
#define GX_TEXT 11 /* committed Unicode scalar in dx */
#define GX_TEXT_KEY 32 /* suppress HID-to-text translation; GX_TEXT follows */
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
/* Read-only desktop cursor, relative to this window's guest pixels. Signed
 * coordinates may lie outside the surface. Does not route input or buttons. */
struct gx_pointer { uint16_t version; uint32_t window; int32_t x, y; };
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
/* Same fit/letterbox operation, with one 0..255 palette index per source
 * cell and a borrowed 256-entry RGB565 palette. Neither pointer is retained. */
struct gx_indexed_upload { struct gx_upload image; const uint16_t *palette; };
/* Synchronous operations on the caller's validated RAM, in RGB565 cells.
 * Copy spans must be disjoint; snapshot overlapping sources before calling.
 * Neither operation publishes a window or retains a pointer. */
struct gx_raster {
    uint16_t version,width,height,dst_stride,src_stride,colour;
    uint16_t *dst;const uint16_t *src;
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
