/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RIPPLE_GX_H
#define RIPPLE_GX_H
#include <stdint.h>
/* Guest ABI: C16 cells, opaque generation handles, never host pointers.
 * All buffers are borrowed only for the duration of the synchronous call. */
#define GX_VERSION 1
/* Hosted surface bounds; the VM desktop advertises its own GX_INFO geometry. */
#ifndef GX_HOSTED_MAX_WIDTH
#define GX_HOSTED_MAX_WIDTH 1920
#endif
#ifndef GX_HOSTED_MAX_HEIGHT
#define GX_HOSTED_MAX_HEIGHT 1080
#endif
#define GX_RGB565 1
/* Opaque eight-bit RGB channels. Colour values are 0x00RRGGBB; the
 * representation of a host framebuffer is deliberately not part of GX. */
#define GX_XRGB8888 2
#define GX_FORMAT_RGB565 1UL
#define GX_FORMAT_XRGB8888 2UL
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
#define GX_CAP_BGR888_UPLOAD 4096
#define GX_CAP_BACKING_SCALE 8192
#define GX_CAP_BGR888_RASTER 16384
#define GX_CAP_PRESENTATION_ZOOM 32768UL
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
/* Optional GX upload using gx_upload: three logical octet cells per pixel,
 * B,G,R order, stride in source cells. Retains all eight channel bits on an
 * XRGB8888 surface; quantizes only on RGB565. Does not publish the surface.
 * The source is borrowed only during this call. */
#define GX_UPLOAD_BGR888 0x4717
/* Opt into 1x/2x backing pixels while window geometry and events stay logical.
 * scale=0 follows display density; 1/2 select a fixed scale. Returns the
 * effective scale and physical surface dimensions. Resize requests also
 * report display-density changes; reapply after acknowledging GX_CONFIGURE. */
#define GX_BACKING_SCALE 0x4718
struct gx_backing {uint16_t version;uint32_t window;uint16_t scale,width,height;};
/* Synchronous BGR logical-octet fill/blend in the caller's RAM. Optional A8
 * mask; row strides count cells, colours and opacity are 0..255. */
#define GX_RASTER_BGR888 0x4719
struct gx_raster_bgr888 {
    uint16_t version,width,height,dst_stride,mask_stride,blue,green,red,opacity;
    uint8_t *dst;const uint8_t *mask;
};
/* Optional format discovery. GX_INFO stays compatible with legacy RGB565
 * clients. If unsupported, use RGB565. formats is a GX_FORMAT_* bit mask;
 * preferred is a GX_* format identifier to request at creation. */
#define GX_FORMATS 0x471a
struct gx_formats {uint16_t version,preferred;uint32_t formats;};
/* Opaque RGB fill, colour=0x00RRGGBB (upper byte must be zero). Quantization
 * occurs only if the destination was explicitly created as RGB565. */
#define GX_FILL_RGB888 0x471b
struct gx_fill_rgb888 {
    uint16_t version;uint32_t surface;
    uint16_t x,y,width,height;uint32_t colour;
};
/* Optional native-window presentation zoom, 100..300 percent. Guest window,
 * raster, upload and input coordinates retain their original dimensions.
 * The host may reduce zoom to fit its monitor; percent returns the effective
 * value. This is independent of GX_BACKING_SCALE and does not resample RAM. */
#define GX_PRESENTATION_ZOOM 0x471c
struct gx_presentation { uint16_t version;uint32_t window;uint16_t percent; };
/* Optional synchronous coverage rasterization into the caller's A8 RAM.
 * Samples are logical octets, one per C16 cell. Coordinates are signed
 * physical pixels and may lie outside the mask; clipping bounds the work.
 * Primitives combine coverage with max(), preserving joins. dot=0 draws an
 * antialiased line (size=1..8); dot=1 a filled circle (radius size=0..8).
 * No window, colour, presentation or application semantics are involved.
 *
 * Version 1 with all other fields zero probes support. Real requests allow
 * at most 8192 primitives, width <= GX_HOSTED_MAX_WIDTH*2 and height <=
 * GX_HOSTED_MAX_HEIGHT*2. Stride counts cells. GX_A8_CLEAR clears the active
 * width of each row before rasterization; padding is never modified. Source
 * and destination spans must be disjoint. All parameters and spans are
 * checked before any destination write; pointers are never retained. */
#define GX_RASTER_A8 0x471d
#define GX_A8_CLEAR 1
struct gx_a8_primitive {
    int32_t x0,y0,x1,y1;
    uint16_t dot,size;
};
struct gx_raster_a8 {
    uint16_t version,width,height,stride,flags;
    uint32_t count;
    uint8_t *dst;
    const struct gx_a8_primitive *primitives;
};
/* Optional retained software rendering. This structure is the C16 ioctl
 * adapter, not a native/LP64 wire layout. Commands contain numeric uint32_t
 * words and payload-relative offsets, never pointers or toolkit objects.
 * A future native adapter must decode its own envelope and preserve these
 * command semantics, pixel formats, ownership and synchronous borrowing.
 *
 * Targets belong to one owned window; destroying the window releases them.
 * Resizing the window does not resize targets. No call presents implicitly.
 * Dimensions/coordinates count physical pixels. Payload lengths/strides count
 * logical octets (one C16 cell each), not host bytes. Only low eight bits of
 * payload cells are used. Complete requests are checked before mutation. */
#define GX_RENDER 0x471e
#define GX_RENDER_PROBE 0
#define GX_RENDER_CREATE 1
#define GX_RENDER_DESTROY 2
#define GX_RENDER_BATCH 3
#define GX_RENDER_RASTER_A8 4
#define GX_RENDER_UPLOAD 5
#define GX_RENDER_READBACK 6
#define GX_RENDER_BLIT 7
#define GX_RENDER_A8 3
#define GX_RENDER_MAX_COMMANDS 256
#define GX_RENDER_MAX_DATA (8UL * 1024UL * 1024UL)
#define GX_RENDER_FILL 1
#define GX_RENDER_A8_DATA 2
#define GX_RENDER_A8_TARGET 3
#define GX_RENDER_COPY 4
#define GX_RENDER_COMMAND_WORDS 12
/* PROBE has version=1, all other fields zero. CREATE takes window,
 * width/height and format (GX_XRGB8888 or GX_RENDER_A8), returning target.
 * DESTROY takes window/target. Unused fields of every request must be zero.
 *
 * BATCH takes window/target, count commands and optional data/data_length.
 * Each command has 12 numeric words: opcode,x,y,width,height,colour,opacity,
 * source,data_stride_or_source_x,source_y,0,0. Rectangles must be in bounds.
 * FILL uses colour=0x00RRGGBB and opacity 0..255; words 7..9 are zero.
 * A8_DATA uses word 7 as payload offset, word 8 as mask stride, word 9 zero.
 * A8_TARGET uses words 7..9 as A8 target handle/source x/source y.
 * COPY uses words 7..9 as colour target handle/source x/source y; colour and
 * opacity are zero. Commands execute in order after complete validation;
 * self-copy has memmove semantics at its position in that order. Count <=256.
 *
 * RASTER_A8 takes an A8 target, flags (GX_A8_CLEAR or zero), count <=8192 and
 * six words per primitive: signed x0,y0,x1,y1, then dot,size, with the same
 * coverage rules as GX_RASTER_A8. It retains coverage in renderer memory.
 *
 * UPLOAD/READBACK take x/y,width/height,stride,data/data_length. A8 uses one
 * logical octet/sample; colour uses three B,G,R logical octets/pixel. Padding
 * is preserved. READBACK does not expose or retain native pointers.
 * BLIT copies x/y,width/height from a colour target to dx/dy in its window,
 * quantizing only for an explicitly RGB565 window. Both rectangles must fit. */
struct gx_render {
    uint16_t version,operation;
    uint32_t window,target;
    uint16_t width,height,format,flags,x,y,dx,dy;
    uint32_t count,stride;
    const uint32_t *commands;
    uint8_t *data;
    uint32_t data_length;
};
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
#define GX_INSIDE 4
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
/* GX_UPLOAD, GX_UPLOAD_SCALED, GX_UPLOAD_INDEXED and GX_FILL retain their
 * RGB565 source/colour semantics on every surface. GX_COPY preserves the
 * destination's native precision. No existing request changes meaning. */
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
