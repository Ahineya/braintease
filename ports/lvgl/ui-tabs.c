/* SPDX-License-Identifier: MIT */
#include "ui-tabs.h"
#include <stdlib.h>
#include <string.h>

struct tab_entry {
    ui_tabs *owner;
    unsigned id;
    char *text;
    int dirty, closable;
    unsigned hover_parts;
    lv_obj_t *row, *button, *label, *close, *dirty_mark, *bridge;
};
struct ui_tabs {
    lv_obj_t *root;
    struct tab_entry *entries;
    unsigned count, active;
    ui_tabs_fn change;
    void *user;
    unsigned dispatch_depth, live_entries, generation;
};
static void release(ui_tabs *tabs) {
    unsigned i;
    if (tabs->dispatch_depth)
        tabs->dispatch_depth--;
    if (tabs->root || tabs->dispatch_depth || tabs->live_entries)
        return;
    for (i = 0; i < tabs->count; i++)
        free(tabs->entries[i].text);
    free(tabs->entries);
    free(tabs);
}
static void activate(ui_tabs *tabs, unsigned id) {
    tabs->dispatch_depth++;
    ui_tabs_select(tabs, id);
    if (tabs->root && tabs->change)
        tabs->change(id, 0, tabs->user);
    release(tabs);
}
static void tab_clicked(lv_event_t *event) {
    struct tab_entry *entry = lv_event_get_user_data(event);
    activate(entry->owner, entry->id);
}
static void tab_closed(lv_event_t *event) {
    struct tab_entry *entry = lv_event_get_user_data(event);
    ui_tabs *tabs = entry->owner;
    unsigned id = entry->id;
    int closable = entry->closable;
    tabs->dispatch_depth++;
    if (closable && tabs->root && tabs->change)
        tabs->change(id, 1, tabs->user);
    release(tabs);
}
static void tab_key(lv_event_t *event) {
    struct tab_entry *entry = lv_event_get_user_data(event);
    ui_tabs *tabs = entry->owner;
    unsigned i, next, generation, id = entry->id;
    uint32_t key = lv_event_get_key(event);
    tabs->dispatch_depth++;
    for (i = 0; i < tabs->count && tabs->entries[i].id != id; i++) {}
    if (i == tabs->count)
        goto done;
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
        goto done;
    } else
        goto done;
    if (next != i && tabs->entries[next].button) {
        id = tabs->entries[next].id;
        generation = tabs->generation;
        lv_group_focus_obj(tabs->entries[next].button);
        /* A focus callback may replace the model or destroy the component. */
        if (tabs->root && tabs->generation == generation)
            activate(tabs, id);
    }
done:
    release(tabs);
}
static void affordance(struct tab_entry *entry) {
    if (!entry->button)
        return;
    lv_obj_set_state(entry->button, LV_STATE_USER_1, entry->hover_parts != 0);
    if (entry->close)
        lv_obj_set_style_opa(entry->close,
                            entry->closable && (entry->hover_parts ||
                            lv_obj_has_state(entry->button, LV_STATE_FOCUS_KEY)) ?
                            LV_OPA_COVER : LV_OPA_TRANSP, 0);
}
static void tab_affordance(lv_event_t *event) {
    struct tab_entry *entry = lv_event_get_user_data(event);
    unsigned part = lv_event_get_target_obj(event) == entry->close ? 2 : 1;
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_STATE_CHANGED &&
        !((lv_event_get_prev_state(event) ^ lv_obj_get_state(entry->button)) & LV_STATE_FOCUS_KEY))
        return;
    /* LVGL sends the new target's HOVER_OVER before the old target's LEAVE.
     * Track both targets so crossing into the close child keeps it visible. */
    if (code == LV_EVENT_HOVER_OVER)
        entry->hover_parts |= part;
    else if (code == LV_EVENT_HOVER_LEAVE)
        entry->hover_parts &= ~part;
    affordance(entry);
}
static void entry_deleted(lv_event_t *event) {
    struct tab_entry *entry = lv_event_get_user_data(event);
    ui_tabs *tabs = entry->owner;
    if (entry->row) {
        if (lv_group_get_focused(gx_lv_group()) == entry->button)
            gx_lv_cancel_key_repeat();
        entry->row = entry->button = entry->label = entry->close = NULL;
        entry->dirty_mark = entry->bridge = NULL;
        tabs->live_entries--;
        tabs->generation++;
    }
    /* Root DELETE precedes child DELETE in LVGL. Keep event userdata alive
     * through every tab's deletion, and through any application callback. */
    tabs->dispatch_depth++;
    release(tabs);
}
static void root_deleted(lv_event_t *event) {
    ui_tabs *tabs = lv_event_get_user_data(event);
    gx_lv_cancel_key_repeat();
    tabs->root = NULL;
    tabs->generation++;
    tabs->dispatch_depth++;
    release(tabs);
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
    if (!tabs->root) {
        free(tabs->entries);
        free(tabs);
        return NULL;
    }
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
    if (!tabs || !tabs->root)
        return;
    for (i = 0; i < tabs->count; i++)
        if (tabs->entries[i].id == id)
            break;
    if (i == tabs->count || !tabs->entries[i].button)
        return;
    tabs->active = id;
    for (i = 0; i < tabs->count; i++) {
        if (!tabs->entries[i].button)
            continue;
        if (tabs->entries[i].id == id) {
            lv_obj_add_state(tabs->entries[i].button, LV_STATE_CHECKED);
            lv_obj_set_style_bg_opa(tabs->entries[i].bridge, LV_OPA_COVER, 0);
        } else
        {
            lv_obj_remove_state(tabs->entries[i].button, LV_STATE_CHECKED);
            lv_obj_set_style_bg_opa(tabs->entries[i].bridge, LV_OPA_TRANSP, 0);
        }
    }
    for (i = 0; i < tabs->count; i++)
        if (tabs->entries[i].id == id && tabs->entries[i].row) {
            lv_obj_scroll_to_view(tabs->entries[i].row, LV_ANIM_OFF);
            break;
        }
}
unsigned ui_tabs_selected(ui_tabs *tabs) { return tabs ? tabs->active : 0; }
int ui_tabs_bind(ui_tabs *tabs, const struct ui_tab_spec *specs, unsigned count,
                 unsigned active) {
    char *text[UI_TABS_LIMIT];
    unsigned i, j;
    int rebuild;
    if (!tabs || !tabs->root || count > UI_TABS_LIMIT || (count && !specs))
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
        text[i] = malloc(n + 1);
        if (!text[i]) {
            for (j = 0; j < i; j++)
                free(text[j]);
            return 0;
        }
        memcpy(text[i], specs[i].text, n + 1);
    }
    rebuild = tabs->count != count;
    for (i = 0; i < count && !rebuild; i++)
        rebuild = tabs->entries[i].id != specs[i].id ||
                  tabs->entries[i].closable != !!specs[i].closable ||
                  !tabs->entries[i].row;
    tabs->generation++;
    if (rebuild) {
        gx_lv_cancel_key_repeat();
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
            entry->row = entry->button = ui_button(tabs->root, NULL, UI_TOOL);
            lv_obj_remove_style_all(entry->button);
            lv_obj_set_width(entry->button, LV_SIZE_CONTENT);
            lv_obj_set_scrollable(entry->button, false);
            lv_obj_set_scroll_on_focus(entry->button, false);
            lv_obj_set_flex_flow(entry->button, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(entry->button, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            entry->close = ui_icon_button(entry->button, NULL, "x", UI_TOOL);
            lv_obj_remove_style_all(entry->close);
            lv_obj_set_scrollable(entry->close, false);
            lv_obj_set_click_focusable(entry->close, false);
            lv_group_remove_obj(entry->close);
            lv_obj_set_clickable(entry->close, entry->closable);
            entry->label = ui_label(entry->button, text[i], UI_BODY);
            lv_label_set_long_mode(entry->label, LV_LABEL_LONG_MODE_DOTS);
            entry->dirty_mark = ui_panel(entry->button);
            lv_obj_remove_style_all(entry->dirty_mark);
            lv_obj_set_clickable(entry->dirty_mark, false);
            lv_obj_set_scrollable(entry->dirty_mark, false);
            entry->bridge = ui_panel(entry->button);
            lv_obj_remove_style_all(entry->bridge);
            lv_obj_set_ignore_layout(entry->bridge, true);
            lv_obj_set_clickable(entry->bridge, false);
            lv_obj_set_scrollable(entry->bridge, false);
            lv_obj_move_to_index(entry->bridge, 0);
            lv_obj_add_event_cb(entry->button, entry_deleted, LV_EVENT_DELETE, entry);
            tabs->live_entries++;
            lv_obj_add_event_cb(entry->button, tab_clicked, LV_EVENT_CLICKED, entry);
            lv_obj_add_event_cb(entry->button, tab_key, LV_EVENT_KEY, entry);
            lv_obj_add_event_cb(entry->button, tab_affordance, LV_EVENT_HOVER_OVER, entry);
            lv_obj_add_event_cb(entry->button, tab_affordance, LV_EVENT_HOVER_LEAVE, entry);
            lv_obj_add_event_cb(entry->button, tab_affordance, LV_EVENT_FOCUSED, entry);
            lv_obj_add_event_cb(entry->button, tab_affordance, LV_EVENT_DEFOCUSED, entry);
            lv_obj_add_event_cb(entry->button, tab_affordance, LV_EVENT_STATE_CHANGED, entry);
            lv_obj_add_event_cb(entry->close, tab_affordance, LV_EVENT_HOVER_OVER, entry);
            lv_obj_add_event_cb(entry->close, tab_affordance, LV_EVENT_HOVER_LEAVE, entry);
            if (entry->closable) {
                lv_obj_add_event_cb(entry->close, tab_closed, LV_EVENT_CLICKED, entry);
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
    if (!tabs || !tabs->root)
        return;
    gx_lv_height(tabs->root, 32);
    lv_obj_set_style_pad_all(tabs->root, 0, 0);
    lv_obj_set_style_pad_top(tabs->root, gx_lv_px(2), 0);
    lv_obj_set_style_pad_column(tabs->root, gx_lv_px(1), 0);
    lv_obj_set_style_bg_color(tabs->root, ui_colour(UI_CANVAS), 0);
    lv_obj_set_style_bg_opa(tabs->root, LV_OPA_COVER, 0);
    for (i = 0; i < tabs->count; i++) {
        struct tab_entry *entry = &tabs->entries[i];
        if (!entry->button)
            continue;
        gx_lv_height(entry->button, 30);
        lv_obj_set_style_min_width(entry->button, gx_lv_px(104), 0);
        lv_obj_set_style_pad_hor(entry->button, gx_lv_px(8), 0);
        lv_obj_set_style_pad_ver(entry->button, 0, 0);
        lv_obj_set_style_pad_column(entry->button, gx_lv_px(6), 0);
        lv_obj_set_style_radius(entry->button, gx_lv_px(7), 0);
        lv_obj_set_style_bg_color(entry->button, ui_colour(UI_PANEL), 0);
        lv_obj_set_style_bg_opa(entry->button, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_opa(entry->button, LV_OPA_COVER, LV_STATE_USER_1);
        lv_obj_set_style_bg_opa(entry->button, LV_OPA_COVER, LV_STATE_CHECKED);
        lv_obj_set_style_border_width(entry->button, gx_lv_px(1), 0);
        lv_obj_set_style_border_side(entry->button, LV_BORDER_SIDE_RIGHT, 0);
        lv_obj_set_style_border_color(entry->button, ui_colour(UI_BORDER), 0);
        lv_obj_set_style_border_opa(entry->button, LV_OPA_50, 0);
        lv_obj_set_style_border_opa(entry->button, LV_OPA_TRANSP, LV_STATE_CHECKED);
        lv_obj_set_style_text_color(entry->button, ui_colour(UI_MUTED), 0);
        lv_obj_set_style_text_color(entry->button, ui_colour(UI_TEXT), LV_STATE_USER_1);
        lv_obj_set_style_text_color(entry->button, ui_colour(UI_TEXT), LV_STATE_CHECKED);
        lv_obj_set_style_outline_width(entry->button, gx_lv_px(1), LV_STATE_FOCUS_KEY);
        lv_obj_set_style_outline_pad(entry->button, -gx_lv_px(1), LV_STATE_FOCUS_KEY);
        lv_obj_set_style_outline_color(entry->button, ui_colour(UI_ACCENT), LV_STATE_FOCUS_KEY);
        lv_obj_set_style_max_width(entry->label, gx_lv_px(180), 0);
        lv_obj_set_height(entry->label,
                          lv_font_get_line_height(lv_obj_get_style_text_font(entry->label,
                                                                            LV_PART_MAIN)));
        gx_lv_size(entry->close, 20, 20);
        lv_obj_set_style_radius(entry->close, gx_lv_px(5), 0);
        lv_obj_set_style_bg_color(entry->close, ui_colour(UI_BORDER), 0);
        lv_obj_set_style_bg_opa(entry->close, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_opa(entry->close, LV_OPA_COVER, LV_STATE_HOVERED);
        lv_obj_set_style_bg_opa(entry->close, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_text_color(entry->close, ui_colour(UI_TEXT), 0);
        lv_obj_center(lv_obj_get_child(entry->close, 0));
        gx_lv_size(entry->dirty_mark, 6, 6);
        lv_obj_set_style_radius(entry->dirty_mark, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(entry->dirty_mark, ui_colour(UI_ACCENT), 0);
        lv_obj_set_style_bg_opa(entry->dirty_mark,
                              entry->dirty ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        gx_lv_height(entry->bridge, 7);
        lv_obj_align(entry->bridge, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(entry->bridge, ui_colour(UI_PANEL), 0);
        lv_obj_set_style_bg_opa(entry->bridge,
                              tabs->active == entry->id ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        affordance(entry);
    }
    lv_obj_update_layout(tabs->root);
    for (i = 0; i < tabs->count; i++)
        if (tabs->entries[i].button)
            lv_obj_set_width(tabs->entries[i].bridge,
                             lv_obj_get_width(tabs->entries[i].button));
}
