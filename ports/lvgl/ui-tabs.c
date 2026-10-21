/* SPDX-License-Identifier: MIT */
#include "ui-tabs.h"
#include <stdlib.h>
#include <string.h>

struct tab_entry {
    ui_tabs *owner;
    unsigned id;
    char *text;
    int dirty, closable;
    lv_obj_t *row, *button, *label, *close;
};
struct ui_tabs {
    lv_obj_t *root;
    struct tab_entry *entries;
    unsigned count, active;
    ui_tabs_fn change;
    void *user;
};
static void activate(ui_tabs *tabs, unsigned id) {
    ui_tabs_select(tabs, id);
    if (tabs->change)
        tabs->change(id, 0, tabs->user);
}
static void tab_clicked(lv_event_t *event) {
    struct tab_entry *entry = lv_event_get_user_data(event);
    activate(entry->owner, entry->id);
}
static void tab_closed(lv_event_t *event) {
    struct tab_entry *entry = lv_event_get_user_data(event);
    ui_tabs *tabs = entry->owner;
    if (entry->closable && tabs->change)
        tabs->change(entry->id, 1, tabs->user);
}
static void tab_key(lv_event_t *event) {
    struct tab_entry *entry = lv_event_get_user_data(event);
    ui_tabs *tabs = entry->owner;
    unsigned i, next;
    uint32_t key = lv_event_get_key(event);
    for (i = 0; i < tabs->count && tabs->entries[i].id != entry->id; i++) {}
    if (i == tabs->count)
        return;
    next = i;
    if (key == LV_KEY_LEFT && i)
        next--;
    else if (key == LV_KEY_RIGHT && i + 1 < tabs->count)
        next++;
    else if (key == LV_KEY_HOME)
        next = 0;
    else if (key == LV_KEY_END)
        next = tabs->count - 1;
    else if (key == LV_KEY_DEL) {
        tab_closed(event);
        return;
    } else
        return;
    if (next != i) {
        lv_group_focus_obj(tabs->entries[next].button);
        activate(tabs, tabs->entries[next].id);
    }
}
static void root_deleted(lv_event_t *event) {
    ui_tabs *tabs = lv_event_get_user_data(event);
    unsigned i;
    for (i = 0; i < tabs->count; i++)
        free(tabs->entries[i].text);
    free(tabs->entries);
    free(tabs);
}
ui_tabs *ui_tabs_create(lv_obj_t *parent, ui_tabs_fn change, void *user) {
    ui_tabs *tabs = calloc(1, sizeof(*tabs));
    if (!tabs)
        return NULL;
    tabs->entries = calloc(UI_TABS_LIMIT, sizeof(*tabs->entries));
    if (!tabs->entries) {
        free(tabs);
        return NULL;
    }
    tabs->root = ui_stack(parent, LV_FLEX_FLOW_ROW, 0);
    tabs->change = change;
    tabs->user = user;
    lv_obj_set_scrollable(tabs->root, true);
    lv_obj_set_scroll_dir(tabs->root, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(tabs->root, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_event_cb(tabs->root, root_deleted, LV_EVENT_DELETE, tabs);
    ui_tabs_density(tabs);
    return tabs;
}
lv_obj_t *ui_tabs_object(ui_tabs *tabs) { return tabs ? tabs->root : NULL; }
void ui_tabs_select(ui_tabs *tabs, unsigned id) {
    unsigned i;
    if (!tabs)
        return;
    for (i = 0; i < tabs->count; i++)
        if (tabs->entries[i].id == id)
            break;
    if (i == tabs->count)
        return;
    tabs->active = id;
    for (i = 0; i < tabs->count; i++) {
        if (tabs->entries[i].id == id) {
            lv_obj_add_state(tabs->entries[i].button, LV_STATE_CHECKED);
            lv_obj_scroll_to_view(tabs->entries[i].row, LV_ANIM_OFF);
        } else
            lv_obj_remove_state(tabs->entries[i].button, LV_STATE_CHECKED);
    }
}
unsigned ui_tabs_selected(ui_tabs *tabs) { return tabs ? tabs->active : 0; }
int ui_tabs_bind(ui_tabs *tabs, const struct ui_tab_spec *specs, unsigned count,
                 unsigned active) {
    char *text[UI_TABS_LIMIT];
    unsigned i, j;
    int rebuild;
    if (!tabs || count > UI_TABS_LIMIT || (count && !specs))
        return 0;
    for (i = 0; i < count; i++) {
        if (!specs[i].text || strlen(specs[i].text) > 2048)
            return 0;
        for (j = 0; j < i; j++)
            if (specs[j].id == specs[i].id)
                return 0;
    }
    if (count) {
        for (i = 0; i < count && specs[i].id != active; i++) {}
        if (i == count)
            return 0;
    }
    memset(text, 0, sizeof(text));
    for (i = 0; i < count; i++) {
        unsigned long n = strlen(specs[i].text);
        text[i] = malloc(n + 3);
        if (!text[i]) {
            for (j = 0; j < i; j++)
                free(text[j]);
            return 0;
        }
        j = 0;
        if (specs[i].dirty) {
            text[i][j++] = '*';
            text[i][j++] = ' ';
        }
        memcpy(text[i] + j, specs[i].text, n);
        text[i][n + j] = 0;
    }
    rebuild = tabs->count != count;
    for (i = 0; i < count && !rebuild; i++)
        rebuild = tabs->entries[i].id != specs[i].id ||
                  tabs->entries[i].closable != !!specs[i].closable;
    if (rebuild) {
        lv_obj_clean(tabs->root);
        for (i = 0; i < tabs->count; i++)
            free(tabs->entries[i].text);
        memset(tabs->entries, 0, UI_TABS_LIMIT * sizeof(*tabs->entries));
        tabs->count = count;
        for (i = 0; i < count; i++) {
            struct tab_entry *entry = &tabs->entries[i];
            entry->owner = tabs;
            entry->id = specs[i].id;
            entry->closable = !!specs[i].closable;
            entry->row = ui_stack(tabs->root, LV_FLEX_FLOW_ROW, 0);
            lv_obj_set_width(entry->row, LV_SIZE_CONTENT);
            entry->button = ui_button(entry->row, NULL, UI_ROW);
            lv_obj_set_width(entry->button, LV_SIZE_CONTENT);
            entry->label = ui_label(entry->button, text[i], UI_BODY);
            lv_label_set_long_mode(entry->label, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_center(entry->label);
            lv_obj_add_event_cb(entry->button, tab_clicked, LV_EVENT_CLICKED, entry);
            lv_obj_add_event_cb(entry->button, tab_key, LV_EVENT_KEY, entry);
            if (entry->closable) {
                entry->close = ui_icon_button(entry->row, NULL, "x", UI_TOOL);
                lv_obj_add_event_cb(entry->close, tab_closed, LV_EVENT_CLICKED, entry);
                lv_obj_add_event_cb(entry->close, tab_key, LV_EVENT_KEY, entry);
            }
        }
    }
    for (i = 0; i < count; i++) {
        struct tab_entry *entry = &tabs->entries[i];
        if (!entry->text || strcmp(entry->text, text[i]))
            lv_label_set_text(entry->label, text[i]);
        free(entry->text);
        entry->text = text[i];
        entry->dirty = !!specs[i].dirty;
    }
    ui_tabs_density(tabs);
    if (count)
        ui_tabs_select(tabs, active);
    else
        tabs->active = 0;
    return 1;
}
void ui_tabs_density(ui_tabs *tabs) {
    unsigned i;
    if (!tabs)
        return;
    gx_lv_height(tabs->root, 36);
    for (i = 0; i < tabs->count; i++) {
        struct tab_entry *entry = &tabs->entries[i];
        gx_lv_height(entry->row, UI_CONTROL);
        gx_lv_height(entry->button, UI_CONTROL);
        lv_obj_set_style_max_width(entry->label, gx_lv_px(180), 0);
        if (entry->close)
            gx_lv_size(entry->close, 26, UI_CONTROL);
    }
}
