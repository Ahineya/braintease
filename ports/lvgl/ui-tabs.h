/* SPDX-License-Identifier: MIT */
#ifndef GRAPHITE_UI_TABS_H
#define GRAPHITE_UI_TABS_H
#include "ui.h"
#define UI_TABS_LIMIT 16
typedef struct ui_tabs ui_tabs;
struct ui_tab_spec {
    unsigned id;
    const char *text;
    int dirty, closable;
};
/* IDs are stable application identities, not LVGL pointers. Labels are copied.
 * User activation selects a tab then calls change(id,0,user); close controls
 * call change(id,1,user) without removing anything. Model and unsaved-work
 * decisions remain with the application. Root deletion owns cleanup. */
typedef void (*ui_tabs_fn)(unsigned id, int close, void *user);
ui_tabs *ui_tabs_create(lv_obj_t *parent, ui_tabs_fn change, void *user);
lv_obj_t *ui_tabs_object(ui_tabs *tabs);
int ui_tabs_bind(ui_tabs *tabs, const struct ui_tab_spec *specs, unsigned count,
                 unsigned active);
void ui_tabs_select(ui_tabs *tabs, unsigned id);
unsigned ui_tabs_selected(ui_tabs *tabs);
void ui_tabs_density(ui_tabs *tabs);
#endif
