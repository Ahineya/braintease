/* SPDX-License-Identifier: MIT */
#ifndef GRAPHITE_UI_APP_H
#define GRAPHITE_UI_APP_H
#include "ui-data.h"

typedef struct ui_actions ui_actions;
/* Strings remain owned by the caller. IDs must be unique and nonzero.
 * Keys use GX USB HID codes; modifiers: bit 0 Control/Command, bit 1 Shift.
 * key=0 means no shortcut. Each nonzero key/modifier pair must be unique. */
struct ui_action_spec {
    unsigned id;
    const char *text, *shortcut, *icon;
    unsigned key, modifiers;
    int enabled;
};
ui_actions *ui_actions_create(const struct ui_action_spec *specs, unsigned count,
                              ui_menu_fn execute, void *user);
/* Detaches surviving buttons too. Safe before or after their parent is deleted. */
void ui_actions_delete(ui_actions *actions);
void ui_actions_enable(ui_actions *actions, unsigned id, int enabled);
int ui_actions_enabled(ui_actions *actions, unsigned id);
/* Buttons, menu activation and shortcuts all recheck the same enabled state.
 * Background actions cannot execute while a shared modal overlay is active. */
int ui_actions_invoke(ui_actions *actions, unsigned id);
int ui_actions_key(ui_actions *actions, unsigned key, int down, unsigned modifiers);
lv_obj_t *ui_actions_button(ui_actions *actions, lv_obj_t *parent, unsigned id,
                            unsigned role);
int ui_actions_menu(ui_actions *actions, lv_obj_t *anchor, const unsigned *ids,
                     unsigned count);
/* For menus mixing shared commands with dynamic entries such as recent files. */
int ui_actions_item(ui_actions *actions, unsigned id, struct ui_menu_item *item);

typedef struct ui_split ui_split;
enum ui_split_axis { UI_SPLIT_HORIZONTAL, UI_SPLIT_VERTICAL };
/* Two persistent, borderless content panes and a focusable separator.
 * Root deletion owns cleanup. Size/minimums are logical points; position is
 * the preferred first-pane share in thousandths, retained when space clamps it. */
ui_split *ui_split_create(lv_obj_t *parent, unsigned axis);
lv_obj_t *ui_split_object(ui_split *split);
lv_obj_t *ui_split_pane(ui_split *split, unsigned pane);
lv_obj_t *ui_split_handle(ui_split *split);
void ui_split_minimum(ui_split *split, unsigned first, unsigned second);
void ui_split_position(ui_split *split, unsigned position);
unsigned ui_split_get_position(ui_split *split);
void ui_split_show_second(ui_split *split, int show);
int ui_split_second_visible(ui_split *split);
void ui_split_density(ui_split *split);
/* The root emits VALUE_CHANGED for user/programmatic position and visibility
 * changes, not ordinary resize/density clamping. Arrows adjust by 2.5%;
 * Home/End reach the limits; double-click restores equal panes. */
#endif
