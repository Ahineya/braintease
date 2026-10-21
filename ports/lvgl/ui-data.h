/* SPDX-License-Identifier: MIT */
#ifndef GRAPHITE_UI_DATA_H
#define GRAPHITE_UI_DATA_H
#include "ui.h"
#define UI_TABLE_COLUMNS 64
typedef struct ui_table ui_table;
typedef const char *(*ui_table_value_fn)(void *, uint32_t, unsigned);
typedef void (*ui_table_select_fn)(void *, uint32_t, unsigned);
typedef void (*ui_table_sort_fn)(void *, unsigned);
/* Optional key policy before default navigation. Return nonzero to consume.
 * Do not destroy the table here; defer activation that can close a window. */
typedef int (*ui_table_key_fn)(void *, uint32_t);
ui_table *ui_table_create(lv_obj_t *parent, ui_table_value_fn value, void *user);
lv_obj_t *ui_table_object(ui_table *table);
/* Public scrolling/focus surface, for activation events and test geometry. */
lv_obj_t *ui_table_input(ui_table *table);
void ui_table_bind(ui_table *table, uint32_t rows, unsigned columns);
void ui_table_heading(ui_table *table, unsigned column, const char *text);
void ui_table_callbacks(ui_table *table, ui_table_select_fn select, ui_table_sort_fn sort);
void ui_table_keys(ui_table *table, ui_table_key_fn key);
void ui_table_select(ui_table *table, uint32_t row, unsigned column, int reveal);
void ui_table_refresh(ui_table *table);
void ui_table_density(ui_table *table);
void ui_table_column_width(ui_table *table, unsigned column, unsigned width);
unsigned ui_table_get_width(ui_table *table, unsigned column);
uint32_t ui_table_row(ui_table *table);
unsigned ui_table_column(ui_table *table);
void ui_table_sort_marker(ui_table *table, int column, int descending);
int ui_table_focused(ui_table *table);
void ui_table_focus(ui_table *table);
/* Freeze user selection, header actions and wheel input during a model commit.
 * Programmatic selection/binding and painting remain available. */
void ui_table_enabled(ui_table *table, int enabled);
void ui_table_page(ui_table *table, int direction);
unsigned ui_table_widget_count(ui_table *table);

/* Exclusive choices built from stock checkboxes. Owns the root's user_data.
 * Arrows move/select; Tab and Shift-Tab traverse ordinary controls. */
lv_obj_t *ui_radio_list(lv_obj_t *parent, const char *const *choices, unsigned count,
                        unsigned selected);
unsigned ui_radio_selected(lv_obj_t *list);
void ui_radio_select(lv_obj_t *list, unsigned selected);

typedef struct ui_dialog ui_dialog;
typedef int (*ui_dialog_fn)(ui_dialog *, unsigned, void *);
typedef int (*ui_dialog_key_fn)(ui_dialog *, unsigned, int, unsigned, void *);
typedef void (*ui_menu_fn)(unsigned, void *);
struct ui_menu_item {
    const char *text, *shortcut, *icon;
    unsigned action;
    int disabled;
};
/* One modal overlay at a time; menu actions run after the menu is dismissed. */
int ui_menu_open(lv_obj_t *anchor, const struct ui_menu_item *items, unsigned count,
                 ui_menu_fn action, void *user);
ui_dialog *ui_dialog_open(const char *title, unsigned width, ui_dialog_fn action, void *user);
lv_obj_t *ui_dialog_body(ui_dialog *dialog);
lv_obj_t *ui_dialog_button(ui_dialog *dialog, const char *text, int primary, unsigned action);
void ui_dialog_submit(ui_dialog *dialog, unsigned action);
/* Optional raw USB-key policy after Escape cancellation, before generic
 * modal editing/default submission. Return nonzero to consume both edges. */
void ui_dialog_keys(ui_dialog *dialog, ui_dialog_key_fn key);
void ui_data_resize(void);
void ui_overlay_input_event(lv_event_t *event);
void ui_overlay_close(void);
/* Release an overlay only when its callback context belongs to this owner. */
int ui_overlay_close_for(void *user);
int ui_overlay_active(void);
void ui_message(const char *title, const char *message);
/* Adapter integration: modal text/clipboard/shortcuts never reach the app behind it. */
int ui_overlay_text(uint32_t scalar);
int ui_overlay_action(unsigned action);
int ui_overlay_key(unsigned code, int down, unsigned modifiers);
void ui_data_density(void);
void ui_data_close(void);
#endif
