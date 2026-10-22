/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RIPLIX_GX_PANEL_H
#define RIPLIX_GX_PANEL_H
#include <stdint.h>
/* Optional manager-owned desktop overlay. The ordinary GX surface remains
 * opaque by default; only these output-coordinate rectangles are composited
 * and hit. GXP_ALPHA enables genuine per-pixel transparency within them.
 * Native-window mode rejects this profile. Probe INFO before creating panels. */
#define GX_PANEL 0x4720
#define GX_PANEL_UPLOAD 0x4721
#define GXP_VERSION 1
#define GXP_INFO 0
#define GXP_CONFIGURE 1
#define GXP_RELEASE 2
#define GXP_KEYBOARD 1
#define GXP_MODAL 2
#define GXP_ALPHA 4
#define GXP_CAP_OVERLAY 4
#define GXP_CAP_ALPHA 8
struct gx_panel_rect {
    uint16_t x,y,width,height;
};
/* 14 C16 cells; native targets must marshal explicitly. INFO accepts zeroed
 * inputs, returning output width/height and GXP_CAP_OVERLAY/GXP_CAP_ALPHA
 * support in flags. GXP_ALPHA requires the alpha capability and an XRGB8888
 * surface. Enabling it clears the panel to transparent until repainted.
 * CONFIGURE requires the caller's live GX_SESSION manager authority and own
 * window, whose logical size equals the output. Hidden configuration permits
 * atomic preparation before mapping. Rectangles must be
 * nonempty, within output bounds. count=0 with NULL regions and zero flags/
 * reservations prepares an inactive overlay role without drawing or input
 * (GXP_ALPHA alone is also permitted for transparent preparation). top/bottom reserve space for maximized
 * ordinary windows and must retain a usable client area. width/height are
 * output-only. A pending output resize returns EAGAIN until the caller has
 * accepted GX_RESIZE with GX_CONFIGURE, then the overlay may be retried.
 * MODAL requires KEYBOARD: outside clicks go to the shell for
 * dismissal, while pixels outside its rectangles still show ordinary apps.
 * RELEASE accepts window=0 or the configured window, all other inputs zero.
 * Manager release, exec/exit or destroying the surface revokes it and hides
 * the old panel surface. Hidden surfaces retain configuration but do not
 * reserve space or intercept input.
 * Handles are not inherited authority, and the overlay is never a dock app.
 */
struct gx_panel {
    uint16_t version,operation;
    uint32_t authority,window;
    uint16_t flags,count,top,bottom,width,height;
    struct gx_panel_rect *regions;
};
/* 13 C16 cells. A manager may upload straight-alpha B,G,R,A logical octets
 * into its GXP_ALPHA XRGB8888 panel's back buffer. Coordinates and stride use
 * backing pixels/cells (including 2x density), matching GX_UPLOAD_BGR888.
 * The complete borrowed source is checked and copied before returning; no
 * guest address is retained. GX_PRESENT publishes color and alpha together.
 * The compositor uses source-over alpha, and alpha >=128 at the pointer
 * sample owns input. Transparent corners pass pointer input to apps below;
 * capture and explicit modal dismissal retain their existing ownership.
 */
struct gx_panel_upload {
    uint16_t version;
    uint32_t authority,window;
    uint16_t x,y,width,height;
    uint32_t stride;
    const unsigned char *pixels;
};
#endif
