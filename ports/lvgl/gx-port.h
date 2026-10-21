/* SPDX-License-Identifier: MIT */
#ifndef LVGL_GX_PORT_H
#define LVGL_GX_PORT_H
#include "lvgl.h"
/* One guest process owns one LVGL context and one GX window. */
int gx_lv_open(const char *title, unsigned width, unsigned height);
int gx_lv_run(void);
void gx_lv_close(void);
void gx_lv_handlers(void (*text)(uint32_t), void (*action)(unsigned),
                    void (*resize)(unsigned, unsigned));
lv_group_t *gx_lv_group(void);
void gx_lv_set_group(lv_group_t *group);
int gx_lv_clipboard(char *text, unsigned capacity, int write);
struct gx_a8_primitive;
/* Optional generic coverage batching; zero selects the portable CPU path.
 * The returned RAM samples remain valid for software drawing. The port may
 * also retain a renderer-owned copy until the next raster call or release;
 * release before modifying or freeing the registered buffer. */
int gx_lv_raster_a8(uint8_t *dst, long width, long height, long stride,
                    const struct gx_a8_primitive *primitives, unsigned long count);
void gx_lv_release_mask(void *base);
/* Optional app policy: window minimum, raw USB shortcuts, and normal exit.
 * Shortcut callback sees both edges; return nonzero to consume the key.
 * modifiers: bit 0 Control/Command, bit 1 Shift. */
void gx_lv_minimum(unsigned width, unsigned height);
void gx_lv_shortcuts(int (*key)(unsigned code, int down, unsigned modifiers));
/* Return 0 to keep the app open while it resolves unsaved documents; 1 permits
 * immediate close. A later confirmed quit calls gx_lv_finish(). */
void gx_lv_close_request(int (*request)(void));
void gx_lv_finish(void);
/* UI geometry is in logical points; LVGL draws in backing pixels. */
unsigned gx_lv_density(void);
int gx_lv_px(int n);
void gx_lv_pos(lv_obj_t *, int x, int y);
void gx_lv_size(lv_obj_t *, int w, int h);
void gx_lv_width(lv_obj_t *, int w);
void gx_lv_height(lv_obj_t *, int h);
void gx_lv_apply_density(unsigned n);
#endif
