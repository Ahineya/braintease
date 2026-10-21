/* SPDX-License-Identifier: MIT */
#include "ui.h"
static unsigned density = 1;
unsigned gx_lv_density(void) { return density; }
int gx_lv_px(int n) { return n * density; }
void gx_lv_pos(lv_obj_t *o, int x, int y) { lv_obj_set_pos(o, gx_lv_px(x), gx_lv_px(y)); }
void gx_lv_size(lv_obj_t *o, int w, int h) { lv_obj_set_size(o, gx_lv_px(w), gx_lv_px(h)); }
void gx_lv_width(lv_obj_t *o, int w) { lv_obj_set_width(o, gx_lv_px(w)); }
void gx_lv_height(lv_obj_t *o, int h) { lv_obj_set_height(o, gx_lv_px(h)); }
void gx_lv_apply_density(unsigned n) {
    density = n;
    lv_display_set_dpi(lv_display_get_default(), 96 * n);
    ui_theme_density();
}
