/* SPDX-License-Identifier: MIT */
#include "ui.h"
/* Pin private access here: theme font roles and clearing a group's focus while
 * retaining its traversal list have no public setters in LVGL 9.6. */
#include "src/themes/lv_theme_private.h"
#include "src/core/lv_group_private.h"
#include "icons/data.h"
#include "ui-data.h"

extern const lv_font_t gx_font_12, gx_font_14, gx_font_16;
extern const lv_font_t gx_font_24, gx_font_28, gx_font_32;

enum {
    SCREEN,
    PANEL,
    STACK,
    BODY,
    SMALL,
    HEADING,
    ERROR_TEXT,
    BUTTON,
    HOVER,
    PRESSED,
    DISABLED,
    FOCUS,
    PRIMARY,
    PRIMARY_HOVER,
    PRIMARY_PRESS,
    QUIET,
    QUIET_HOVER,
    ROW,
    ROW_HOVER,
    ROW_PRESS,
    SELECTED,
    SELECTED_HOVER,
    INPUT,
    INPUT_FOCUS,
    INPUT_ERROR,
    CURSOR,
    CURSOR_HIDDEN,
    DISABLED_TEXT,
    SELECTION,
    CHECK,
    CHECKED,
    SCROLLBAR,
    LIST,
    SWATCH,
    ICON,
    ICON_BUTTON,
    ROW_LABEL,
    STYLE_COUNT
};
static lv_style_t styles[STYLE_COUNT];
static lv_theme_t *theme;
static unsigned ready;
static void pointer_pressed(lv_event_t *event) {
    lv_obj_t *target = lv_event_get_param(event);
    lv_group_t *group = gx_lv_group();
    lv_obj_t *focused;
    /* Keep control clicks, disabled controls, list selection and dragging on
     * LVGL's normal path. Only a fresh press on an ungrouped plain surface
     * releases focus; no object is removed/re-added or reordered for Tab. */
    if (!target || !lv_obj_check_type(target, &lv_obj_class) || lv_obj_get_group(target) ||
        !group || group->frozen || !(focused = lv_group_get_focused(group)))
        return;
    ui_focus_clear(group);
}
int ui_focus_clear(lv_group_t *group) {
    lv_obj_t *focused;
    if (!group || group->frozen || !(focused = lv_group_get_focused(group)))
        return 0;
    group->obj_focus = NULL;
    group->editing = 0;
    if (lv_obj_send_event(focused, LV_EVENT_DEFOCUSED, lv_indev_active()) == LV_RESULT_OK)
        lv_obj_invalidate(focused);
    if (group->focus_cb)
        group->focus_cb(group);
    return 1;
}
void ui_attach_pointer(lv_indev_t *pointer) {
    lv_indev_add_event_cb(pointer, pointer_pressed, LV_EVENT_PRESSED, NULL);
}
static void input_defocused(lv_event_t *event) {
    if (lv_event_get_code(event) == LV_EVENT_DEFOCUSED)
        lv_textarea_clear_selection(lv_event_get_target_obj(event));
    ui_overlay_input_event(event);
}
static const uint32_t palette[] = {0x191a1cUL, 0x212225UL, 0x41444bUL,
                                   0xe6e7e9UL, 0xa2a5acUL, 0x9ab8efUL};
lv_color_t ui_colour(unsigned role) {
    return lv_color_hex(palette[role <= UI_ACCENT ? role : UI_TEXT]);
}
const lv_font_t *ui_font(unsigned role) {
    if (role == UI_SECONDARY)
        return gx_lv_density() == 2 ? &gx_font_24 : &gx_font_12;
    if (role == UI_HEADING)
        return gx_lv_density() == 2 ? &gx_font_32 : &gx_font_16;
    return gx_lv_density() == 2 ? &gx_font_28 : &gx_font_14;
}
static void bg(unsigned s, uint32_t colour) {
    lv_style_set_bg_color(&styles[s], lv_color_hex(colour));
    lv_style_set_bg_opa(&styles[s], LV_OPA_COVER);
}
static void ink(unsigned s, uint32_t colour) {
    lv_style_set_text_color(&styles[s], lv_color_hex(colour));
}
static void border(unsigned s, uint32_t colour) {
    lv_style_set_border_color(&styles[s], lv_color_hex(colour));
    lv_style_set_border_width(&styles[s], gx_lv_px(1));
}
static void apply(lv_theme_t *th, lv_obj_t *obj) {
    (void)th;
    if (!lv_obj_get_parent(obj)) {
        lv_obj_add_style(obj, &styles[SCREEN], 0);
        lv_obj_set_click_focusable(obj, false);
    } else if (lv_obj_check_type(obj, &lv_button_class)) {
        lv_obj_add_style(obj, &styles[BUTTON], 0);
        lv_obj_add_style(obj, &styles[HOVER], LV_STATE_HOVERED);
        lv_obj_add_style(obj, &styles[PRESSED], LV_STATE_PRESSED);
        lv_obj_add_style(obj, &styles[DISABLED], LV_STATE_DISABLED);
        lv_obj_add_style(obj, &styles[FOCUS], LV_STATE_FOCUS_KEY);
    } else if (lv_obj_check_type(obj, &lv_textarea_class)) {
        lv_obj_add_event_cb(obj, input_defocused, LV_EVENT_ALL, NULL);
        lv_obj_add_style(obj, &styles[INPUT], 0);
        lv_obj_add_style(obj, &styles[INPUT_FOCUS], LV_STATE_FOCUSED);
        lv_obj_add_style(obj, &styles[INPUT_ERROR], LV_STATE_USER_1);
        lv_obj_add_style(obj, &styles[DISABLED], LV_STATE_DISABLED);
        lv_obj_add_style(obj, &styles[CURSOR_HIDDEN], LV_PART_CURSOR);
        lv_obj_add_style(obj, &styles[CURSOR], LV_PART_CURSOR | LV_STATE_FOCUSED);
        lv_obj_add_style(obj, &styles[SMALL], LV_PART_TEXTAREA_PLACEHOLDER);
        lv_obj_add_style(obj, &styles[SELECTION], LV_PART_SELECTED);
    } else if (lv_obj_check_type(obj, &lv_checkbox_class)) {
        lv_obj_add_style(obj, &styles[STACK], 0);
        lv_obj_add_style(obj, &styles[CHECK], LV_PART_INDICATOR);
        lv_obj_add_style(obj, &styles[CHECKED], LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_add_style(obj, &styles[CHECKED],
                         LV_PART_INDICATOR | LV_STATE_CHECKED | LV_STATE_HOVERED);
        lv_obj_add_style(obj, &styles[HOVER], LV_PART_INDICATOR | LV_STATE_HOVERED);
        lv_obj_add_style(obj, &styles[FOCUS], LV_STATE_FOCUS_KEY);
        lv_obj_add_style(obj, &styles[DISABLED_TEXT], LV_STATE_DISABLED);
        lv_obj_add_style(obj, &styles[DISABLED], LV_PART_INDICATOR | LV_STATE_DISABLED);
    } else if (lv_obj_check_type(obj, &lv_obj_class)) {
        lv_obj_add_style(obj, &styles[PANEL], 0);
        lv_obj_add_style(obj, &styles[SCROLLBAR], LV_PART_SCROLLBAR);
        lv_obj_set_click_focusable(obj, false);
    }
}
void ui_theme_density(void) {
    unsigned i;
    for (i = 0; i < STYLE_COUNT; i++) {
        if (ready)
            lv_style_reset(&styles[i]);
        else
            lv_style_init(&styles[i]);
    }
    bg(SCREEN, palette[UI_CANVAS]);
    ink(SCREEN, palette[UI_TEXT]);
    lv_style_set_text_font(&styles[SCREEN], ui_font(UI_BODY));
    lv_style_set_pad_all(&styles[SCREEN], gx_lv_px(UI_INSET));
    lv_style_set_pad_row(&styles[SCREEN], gx_lv_px(UI_SPACE));
    lv_style_set_pad_column(&styles[SCREEN], gx_lv_px(UI_SPACE));
    bg(PANEL, palette[UI_PANEL]);
    border(PANEL, 0x34363bUL);
    lv_style_set_radius(&styles[PANEL], gx_lv_px(4));
    lv_style_set_pad_all(&styles[PANEL], gx_lv_px(UI_INSET));
    lv_style_set_pad_row(&styles[PANEL], gx_lv_px(UI_SPACE));
    lv_style_set_pad_column(&styles[PANEL], gx_lv_px(UI_SPACE));
    lv_style_set_shadow_width(&styles[PANEL], 0);
    lv_style_set_bg_opa(&styles[STACK], LV_OPA_TRANSP);
    lv_style_set_border_width(&styles[STACK], 0);
    lv_style_set_radius(&styles[STACK], 0);
    lv_style_set_pad_all(&styles[STACK], 0);
    lv_style_set_pad_column(&styles[STACK], gx_lv_px(UI_SPACE));
    lv_style_set_pad_row(&styles[STACK], gx_lv_px(UI_SPACE));
    lv_style_set_text_font(&styles[BODY], ui_font(UI_BODY));
    lv_style_set_height(&styles[ROW_LABEL], lv_font_get_line_height(ui_font(UI_BODY)));
    lv_style_set_text_font(&styles[SMALL], ui_font(UI_SECONDARY));
    ink(SMALL, palette[UI_MUTED]);
    lv_style_set_text_font(&styles[HEADING], ui_font(UI_HEADING));
    ink(HEADING, palette[UI_TEXT]);
    ink(ERROR_TEXT, 0xf0a5a5UL);
    bg(BUTTON, 0x2b2d31UL);
    ink(BUTTON, palette[UI_TEXT]);
    border(BUTTON, palette[UI_BORDER]);
    lv_style_set_radius(&styles[BUTTON], gx_lv_px(4));
    lv_style_set_pad_hor(&styles[BUTTON], gx_lv_px(10));
    lv_style_set_pad_ver(&styles[BUTTON], gx_lv_px(4));
    lv_style_set_height(&styles[BUTTON], gx_lv_px(UI_CONTROL));
    lv_style_set_min_width(&styles[BUTTON], gx_lv_px(64));
    lv_style_set_shadow_width(&styles[BUTTON], 0);
    lv_style_set_outline_width(&styles[BUTTON], 0);
    lv_style_set_anim_duration(&styles[BUTTON], 0);
    bg(HOVER, 0x35383dUL);
    border(HOVER, 0x565b65UL);
    bg(PRESSED, 0x24262aUL);
    border(PRESSED, 0x727985UL);
    bg(DISABLED, 0x25262aUL);
    ink(DISABLED, 0x777b84UL);
    border(DISABLED, 0x35373dUL);
    lv_style_set_text_opa(&styles[DISABLED], LV_OPA_COVER);
    lv_style_set_outline_width(&styles[FOCUS], gx_lv_px(1));
    /* Draw focus on the control edge, inside tightly packed layout bounds. */
    lv_style_set_outline_pad(&styles[FOCUS], -gx_lv_px(1));
    lv_style_set_outline_color(&styles[FOCUS], ui_colour(UI_ACCENT));
    lv_style_set_border_color(&styles[FOCUS], ui_colour(UI_ACCENT));
    bg(PRIMARY, 0x7396ccUL);
    ink(PRIMARY, 0x101723UL);
    border(PRIMARY, 0x7396ccUL);
    bg(PRIMARY_HOVER, 0x86a7dcUL);
    border(PRIMARY_HOVER, 0x9ab8efUL);
    bg(PRIMARY_PRESS, 0x6688beUL);
    border(PRIMARY_PRESS, 0x9ab8efUL);
    bg(QUIET, palette[UI_PANEL]);
    border(QUIET, palette[UI_PANEL]);
    bg(QUIET_HOVER, 0x2d3035UL);
    border(QUIET_HOVER, 0x41444bUL);
    bg(ROW, palette[UI_PANEL]);
    /* The opaque list/panel already paints this colour. Only highlighted rows
     * need their own background; avoid filling every ordinary row twice. */
    lv_style_set_bg_opa(&styles[ROW], LV_OPA_TRANSP);
    lv_style_set_border_width(&styles[ROW], 0);
    lv_style_set_height(&styles[ROW], gx_lv_px(UI_ROW_HEIGHT));
    lv_style_set_radius(&styles[ROW], 0);
    lv_style_set_pad_all(&styles[ROW], 0);
    lv_style_set_pad_hor(&styles[ROW], gx_lv_px(8));
    lv_style_set_pad_column(&styles[ROW], gx_lv_px(UI_SPACE));
    bg(SELECTED, 0x303d52UL);
    lv_style_set_border_width(&styles[SELECTED], 0);
    bg(SELECTED_HOVER, 0x394963UL);
    lv_style_set_border_width(&styles[SELECTED_HOVER], 0);
    bg(ROW_HOVER, 0x2d3035UL);
    lv_style_set_border_width(&styles[ROW_HOVER], 0);
    bg(ROW_PRESS, 0x24262aUL);
    lv_style_set_border_width(&styles[ROW_PRESS], 0);
    bg(INPUT, palette[UI_CANVAS]);
    border(INPUT, palette[UI_BORDER]);
    ink(INPUT, palette[UI_TEXT]);
    lv_style_set_radius(&styles[INPUT], gx_lv_px(4));
    lv_style_set_pad_hor(&styles[INPUT], gx_lv_px(8));
    lv_style_set_pad_ver(&styles[INPUT], gx_lv_px(4));
    lv_style_set_height(&styles[INPUT], gx_lv_px(UI_CONTROL));
    lv_style_set_border_color(&styles[INPUT_FOCUS], ui_colour(UI_ACCENT));
    lv_style_set_border_color(&styles[INPUT_ERROR], lv_color_hex(0xe39292UL));
    lv_style_set_border_side(&styles[CURSOR], LV_BORDER_SIDE_LEFT);
    lv_style_set_border_width(&styles[CURSOR], gx_lv_px(1));
    lv_style_set_border_color(&styles[CURSOR], ui_colour(UI_TEXT));
    lv_style_set_bg_opa(&styles[CURSOR], LV_OPA_TRANSP);
    lv_style_set_anim_duration(&styles[CURSOR], 0);
    lv_style_set_bg_opa(&styles[CURSOR_HIDDEN], LV_OPA_TRANSP);
    lv_style_set_border_width(&styles[CURSOR_HIDDEN], 0);
    lv_style_set_anim_duration(&styles[CURSOR_HIDDEN], 0);
    ink(DISABLED_TEXT, 0x777b84UL);
    bg(SELECTION, 0x394e70UL);
    ink(SELECTION, palette[UI_TEXT]);
    bg(CHECK, palette[UI_CANVAS]);
    border(CHECK, 0x737984UL);
    lv_style_set_radius(&styles[CHECK], gx_lv_px(2));
    lv_style_set_pad_all(&styles[CHECK], 0);
    lv_style_set_text_font(&styles[CHECK], ui_font(UI_BODY));
    bg(CHECKED, 0x7396ccUL);
    border(CHECKED, 0x7396ccUL);
    ink(CHECKED, 0x101723UL);
    lv_style_set_bg_image_src(&styles[CHECKED], LV_SYMBOL_OK);
    bg(SCROLLBAR, 0x626772UL);
    lv_style_set_width(&styles[SCROLLBAR], gx_lv_px(5));
    lv_style_set_radius(&styles[SCROLLBAR], gx_lv_px(2));
    lv_style_set_pad_all(&styles[LIST], gx_lv_px(3));
    lv_style_set_pad_row(&styles[LIST], 0);
    lv_style_set_width(&styles[SWATCH], gx_lv_px(24));
    lv_style_set_height(&styles[SWATCH], gx_lv_px(24));
    lv_style_set_pad_all(&styles[SWATCH], 0);
    lv_style_set_text_font(&styles[ICON], gx_lv_density() == 2 ? &ui_icons_32 : &ui_icons_16);
    lv_style_set_width(&styles[ICON], gx_lv_px(16));
    lv_style_set_height(&styles[ICON], gx_lv_px(16));
    lv_style_set_text_letter_space(&styles[ICON], 0);
    lv_style_set_text_line_space(&styles[ICON], 0);
    lv_style_set_pad_column(&styles[ICON_BUTTON], gx_lv_px(UI_SPACE));
    if (!ready) {
        theme = lv_theme_create();
        lv_theme_set_apply_cb(theme, apply);
        lv_display_set_theme(lv_display_get_default(), theme);
    }
    theme->font_small = ui_font(UI_SECONDARY);
    theme->font_normal = ui_font(UI_BODY);
    theme->font_large = ui_font(UI_HEADING);
    theme->color_primary = ui_colour(UI_ACCENT);
    theme->color_secondary = ui_colour(UI_MUTED);
    if (!ready)
        lv_theme_apply(lv_screen_active());
    ready = 1;
    ui_data_density();
    lv_obj_report_style_change(NULL);
    ui_data_resize();
}
void ui_theme_close(void) {
    unsigned i;
    if (!ready)
        return;
    ui_overlay_close();
    lv_obj_clean(lv_screen_active());
    ui_data_close();
    lv_obj_remove_style_all(lv_screen_active());
    lv_display_set_theme(lv_display_get_default(), NULL);
    lv_theme_delete(theme);
    theme = NULL;
    for (i = 0; i < STYLE_COUNT; i++)
        lv_style_reset(&styles[i]);
    ready = 0;
}
void ui_screen(void) { lv_obj_set_scrollable(lv_screen_active(), false); }
lv_obj_t *ui_stack(lv_obj_t *parent, lv_flex_flow_t flow, unsigned gap) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_add_style(obj, &styles[STACK], 0);
    lv_obj_set_flex_flow(obj, flow);
    /* Standard spacing comes from a shared density-aware style. */
    if (!gap) {
        lv_obj_set_style_pad_row(obj, 0, 0);
        lv_obj_set_style_pad_column(obj, 0, 0);
    }
    lv_obj_set_width(obj, lv_pct(100));
    lv_obj_set_height(obj, LV_SIZE_CONTENT);
    lv_obj_set_scrollable(obj, false);
    return obj;
}
lv_obj_t *ui_panel(lv_obj_t *parent) { return lv_obj_create(parent); }
lv_obj_t *ui_swatch(lv_obj_t *parent, unsigned colour) {
    lv_obj_t *obj = ui_panel(parent);
    lv_obj_add_style(obj, &styles[SWATCH], 0);
    lv_obj_set_style_bg_color(obj, ui_colour(colour), 0);
    lv_obj_set_scrollable(obj, false);
    return obj;
}
void ui_text_role(lv_obj_t *obj, unsigned role) {
    lv_obj_add_style(obj,
                     &styles[role == UI_SECONDARY ? SMALL
                             : role == UI_HEADING ? HEADING
                                                  : BODY],
                     0);
    if (role == UI_ERROR)
        lv_obj_add_style(obj, &styles[ERROR_TEXT], 0);
}
lv_obj_t *ui_label(lv_obj_t *parent, const char *text, unsigned role) {
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    ui_text_role(obj, role);
    return obj;
}
void ui_button_role(lv_obj_t *obj, unsigned role) {
    if (role == UI_PRIMARY) {
        lv_obj_add_style(obj, &styles[PRIMARY], 0);
        lv_obj_add_style(obj, &styles[PRIMARY_HOVER], LV_STATE_HOVERED);
        lv_obj_add_style(obj, &styles[PRIMARY_PRESS], LV_STATE_PRESSED);
    } else if (role == UI_TOOL || role == UI_ROW) {
        lv_obj_add_style(obj, &styles[role == UI_ROW ? ROW : QUIET], 0);
        lv_obj_add_style(obj, &styles[QUIET_HOVER], LV_STATE_HOVERED);
        if (role == UI_ROW) {
            lv_obj_add_style(obj, &styles[ROW_HOVER], LV_STATE_HOVERED);
            lv_obj_add_style(obj, &styles[ROW_PRESS], LV_STATE_PRESSED);
            lv_obj_add_style(obj, &styles[SELECTED], LV_STATE_CHECKED);
            lv_obj_add_style(obj, &styles[SELECTED_HOVER], LV_STATE_CHECKED | LV_STATE_HOVERED);
        }
    }
}
lv_obj_t *ui_button(lv_obj_t *parent, const char *text, unsigned role) {
    lv_obj_t *obj = lv_button_create(parent);
    ui_button_role(obj, role);
    if (text) {
        lv_obj_t *label = ui_label(obj, text, UI_BODY);
        lv_obj_center(label);
    }
    return obj;
}
lv_obj_t *ui_icon(lv_obj_t *parent, const char *name) {
    const char *text = ui_icon_text(name);
    lv_obj_t *obj;
    if (!text)
        return NULL;
    obj = lv_label_create(parent);
    lv_label_set_text_static(obj, text);
    lv_obj_add_style(obj, &styles[ICON], 0);
    lv_obj_set_clickable(obj, false);
    lv_obj_set_click_focusable(obj, false);
    lv_obj_set_scrollable(obj, false);
    return obj;
}
int ui_icon_set(lv_obj_t *icon, const char *name) {
    const char *text = ui_icon_text(name);
    if (!text || !icon)
        return 0;
    lv_label_set_text_static(icon, text);
    return 1;
}
lv_obj_t *ui_icon_button(lv_obj_t *parent, const char *text, const char *icon, unsigned role) {
    lv_obj_t *obj;
    if (!ui_icon_text(icon))
        return NULL;
    obj = ui_button(parent, NULL, role);
    lv_obj_add_style(obj, &styles[ICON_BUTTON], 0);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui_icon(obj, icon);
    if (text)
        ui_label(obj, text, UI_BODY);
    return obj;
}
void ui_input_height(lv_obj_t *obj, unsigned height) {
    int32_t pad = (gx_lv_px(height) - 2 * gx_lv_px(1) -
                   lv_font_get_line_height(lv_obj_get_style_text_font(obj, LV_PART_MAIN))) /
                  2;
    lv_obj_set_style_pad_ver(obj, pad > 0 ? pad : 0, 0);
    gx_lv_height(obj, height);
}
lv_obj_t *ui_input(lv_obj_t *parent, const char *placeholder) {
    lv_obj_t *obj = lv_textarea_create(parent);
    lv_textarea_set_one_line(obj, true);
    lv_textarea_set_text(obj, "");
    if (placeholder)
        lv_textarea_set_placeholder_text(obj, placeholder);
    ui_input_height(obj, UI_CONTROL);
    return obj;
}
void ui_input_error(lv_obj_t *obj, int error) {
    if (error)
        lv_obj_add_state(obj, LV_STATE_USER_1);
    else
        lv_obj_remove_state(obj, LV_STATE_USER_1);
}
lv_obj_t *ui_checkbox(lv_obj_t *parent, const char *text) {
    lv_obj_t *obj = lv_checkbox_create(parent);
    lv_checkbox_set_text(obj, text);
    return obj;
}
lv_obj_t *ui_list(lv_obj_t *parent) {
    lv_obj_t *obj = ui_panel(parent);
    lv_obj_set_click_focusable(obj, true);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_style(obj, &styles[LIST], 0);
    lv_obj_set_scroll_dir(obj, LV_DIR_VER);
    lv_obj_set_scroll_with_arrow(obj, false);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_style(obj, &styles[FOCUS], LV_STATE_FOCUS_KEY);
    return obj;
}
void ui_row_labels(lv_obj_t *row, const char *name, const char *detail) {
    ui_row_icon_labels(row, NULL, name, detail);
}
void ui_row_icon_labels(lv_obj_t *row, const char *icon, const char *name, const char *detail) {
    lv_obj_t *label, *trailing;
    lv_obj_set_scrollable(row, false);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (icon)
        ui_icon(row, icon);
    label = ui_label(row, name, UI_BODY);
    lv_obj_add_style(label, &styles[ROW_LABEL], 0);
    lv_obj_set_flex_grow(label, 1);
    lv_obj_set_width(label, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    trailing = ui_label(row, detail, UI_SECONDARY);
    lv_obj_set_style_text_align(trailing, LV_TEXT_ALIGN_RIGHT, 0);
}
