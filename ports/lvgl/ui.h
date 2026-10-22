/* SPDX-License-Identifier: MIT */
#ifndef GRAPHITE_UI_H
#define GRAPHITE_UI_H
#include "gx-port.h"

/* Semantic roles; application code does not choose individual colours/fonts. */
enum ui_text_role { UI_BODY, UI_SECONDARY, UI_HEADING, UI_ERROR };
enum ui_button_role { UI_BUTTON, UI_PRIMARY, UI_TOOL, UI_ROW };
enum ui_metric { UI_SPACE = 8, UI_INSET = 12, UI_CONTROL = 30, UI_ROW_HEIGHT = 28 };
enum ui_colour_role { UI_CANVAS, UI_PANEL, UI_BORDER, UI_TEXT, UI_MUTED, UI_ACCENT };
lv_color_t ui_colour(unsigned role);
const lv_font_t *ui_font(unsigned role);
/* Called by the window adapter. Shared styles retain their identity on density changes. */
void ui_theme_density(void);
void ui_theme_close(void);
/* Desktop pointer policy: blank surfaces release keyboard focus. */
void ui_attach_pointer(lv_indev_t *pointer);
int ui_focus_clear(lv_group_t *group);
/* Window deactivation changes decoration, not the logical control/selection.
 * Repeated suspend calls are inert unless background code changes focus.
 * Resume restores only a live, enabled, visible target in the same group. */
void ui_focus_suspend(lv_group_t *group);
void ui_focus_resume(lv_group_t *group);
void ui_screen(void);
lv_obj_t *ui_stack(lv_obj_t *parent, lv_flex_flow_t flow, unsigned gap);
lv_obj_t *ui_panel(lv_obj_t *parent);
lv_obj_t *ui_swatch(lv_obj_t *parent, unsigned colour);
lv_obj_t *ui_label(lv_obj_t *parent, const char *text, unsigned role);
lv_obj_t *ui_button(lv_obj_t *parent, const char *text, unsigned role);
/* Named Lucide assets. Icons inherit their parent's text colour and never take focus. */
unsigned ui_icon_count(void);
const char *ui_icon_name(unsigned index);
lv_obj_t *ui_icon(lv_obj_t *parent, const char *name);
int ui_icon_set(lv_obj_t *icon, const char *name);
lv_obj_t *ui_icon_button(lv_obj_t *parent, const char *text, const char *icon, unsigned role);
lv_obj_t *ui_input(lv_obj_t *parent, const char *placeholder);
lv_obj_t *ui_checkbox(lv_obj_t *parent, const char *text);
lv_obj_t *ui_list(lv_obj_t *parent);
void ui_text_role(lv_obj_t *obj, unsigned role);
void ui_button_role(lv_obj_t *obj, unsigned role);
/* Width is owned by the surrounding layout. Height and padding fit a full font line. */
void ui_input_height(lv_obj_t *obj, unsigned height);
void ui_input_error(lv_obj_t *obj, int error);
void ui_row_labels(lv_obj_t *row, const char *name, const char *detail);
void ui_row_icon_labels(lv_obj_t *row, const char *icon, const char *name, const char *detail);
#endif
