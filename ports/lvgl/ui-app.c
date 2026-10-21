/* SPDX-License-Identifier: MIT */
#include "ui-app.h"
#include <stdlib.h>
#include <string.h>

#define ACTION_LIMIT 64
struct action_entry {
    struct ui_action_spec spec;
};
struct action_binding {
    struct action_binding *next;
    ui_actions *owner;
    lv_obj_t *button;
    unsigned id;
};
struct ui_actions {
    struct action_entry *entries;
    struct action_binding *bindings;
    unsigned count;
    ui_menu_fn execute;
    void *user;
};
static struct action_entry *find_action(ui_actions *a, unsigned id) {
    unsigned i;
    if (a)
        for (i = 0; i < a->count; i++)
            if (a->entries[i].spec.id == id)
                return &a->entries[i];
    return NULL;
}
ui_actions *ui_actions_create(const struct ui_action_spec *specs, unsigned count,
                              ui_menu_fn execute, void *user) {
    ui_actions *a;
    unsigned i, j;
    if (!specs || !count || count > ACTION_LIMIT || !execute)
        return NULL;
    for (i = 0; i < count; i++) {
        if (!specs[i].id || !specs[i].text || specs[i].key > 255 || specs[i].modifiers > 3)
            return NULL;
        for (j = 0; j < i; j++)
            if (specs[i].id == specs[j].id ||
                (specs[i].key && specs[i].key == specs[j].key &&
                 specs[i].modifiers == specs[j].modifiers))
                return NULL;
    }
    a = calloc(1, sizeof(*a));
    if (!a)
        return NULL;
    a->entries = calloc(count, sizeof(*a->entries));
    if (!a->entries) {
        free(a);
        return NULL;
    }
    a->count = count;
    a->execute = execute;
    a->user = user;
    for (i = 0; i < count; i++)
        a->entries[i].spec = specs[i];
    return a;
}
static void binding_event(lv_event_t *event) {
    struct action_binding *b = lv_event_get_user_data(event);
    if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        struct action_binding **at = &b->owner->bindings;
        while (*at && *at != b)
            at = &(*at)->next;
        if (*at)
            *at = b->next;
        free(b);
    } else if (lv_event_get_code(event) == LV_EVENT_CLICKED)
        ui_actions_invoke(b->owner, b->id);
}
void ui_actions_delete(ui_actions *a) {
    struct action_binding *b;
    if (!a)
        return;
    ui_overlay_close_for(a);
    while ((b = a->bindings)) {
        a->bindings = b->next;
        lv_obj_remove_event_cb_with_user_data(b->button, binding_event, b);
        lv_obj_add_state(b->button, LV_STATE_DISABLED);
        free(b);
    }
    free(a->entries);
    free(a);
}
void ui_actions_enable(ui_actions *a, unsigned id, int enabled) {
    struct action_entry *entry = find_action(a, id);
    struct action_binding *b;
    if (!entry)
        return;
    entry->spec.enabled = !!enabled;
    for (b = a->bindings; b; b = b->next)
        if (b->id == id) {
            if (enabled)
                lv_obj_remove_state(b->button, LV_STATE_DISABLED);
            else
                lv_obj_add_state(b->button, LV_STATE_DISABLED);
        }
}
int ui_actions_enabled(ui_actions *a, unsigned id) {
    struct action_entry *entry = find_action(a, id);
    return entry && entry->spec.enabled;
}
int ui_actions_invoke(ui_actions *a, unsigned id) {
    if (!ui_actions_enabled(a, id) || ui_overlay_active())
        return 0;
    a->execute(id, a->user);
    return 1;
}
int ui_actions_key(ui_actions *a, unsigned key, int down, unsigned modifiers) {
    unsigned i;
    if (!a || !key)
        return 0;
    for (i = 0; i < a->count; i++) {
        struct ui_action_spec *s = &a->entries[i].spec;
        if (s->key == key && s->modifiers == modifiers) {
            if (down)
                ui_actions_invoke(a, s->id);
            return 1;
        }
    }
    return 0;
}
lv_obj_t *ui_actions_button(ui_actions *a, lv_obj_t *parent, unsigned id, unsigned role) {
    struct action_entry *entry = find_action(a, id);
    struct action_binding *b;
    lv_obj_t *button;
    if (!entry)
        return NULL;
    b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    button = entry->spec.icon ? ui_icon_button(parent, entry->spec.text, entry->spec.icon, role)
                             : ui_button(parent, entry->spec.text, role);
    if (!button) {
        free(b);
        return NULL;
    }
    b->owner = a;
    b->button = button;
    b->id = id;
    b->next = a->bindings;
    a->bindings = b;
    lv_obj_add_event_cb(button, binding_event, LV_EVENT_ALL, b);
    if (!entry->spec.enabled)
        lv_obj_add_state(button, LV_STATE_DISABLED);
    return button;
}
/* The shared menu dismisses its modal before calling this handler. */
static void menu_action(unsigned id, void *user) { ui_actions_invoke(user, id); }
int ui_actions_item(ui_actions *a, unsigned id, struct ui_menu_item *item) {
    struct action_entry *entry = find_action(a, id);
    if (!entry || !item)
        return 0;
    *item = (struct ui_menu_item){entry->spec.text, entry->spec.shortcut, entry->spec.icon,
                                  id, !entry->spec.enabled};
    return 1;
}
int ui_actions_menu(ui_actions *a, lv_obj_t *anchor, const unsigned *ids, unsigned count) {
    struct ui_menu_item *items;
    unsigned i;
    int result;
    if (!a || !anchor || !ids || !count || count > ACTION_LIMIT)
        return 0;
    items = calloc(count, sizeof(*items));
    if (!items)
        return 0;
    for (i = 0; i < count; i++) {
        if (!ui_actions_item(a, ids[i], &items[i])) {
            free(items);
            return 0;
        }
    }
    result = ui_menu_open(anchor, items, count, menu_action, a);
    free(items);
    return result;
}

struct ui_split {
    lv_obj_t *root, *panes[2], *handle;
    unsigned axis, position, minimum[2], visible, busy, dragging;
    int32_t grab, press_axis;
};
static void split_layout(ui_split *s) {
    int32_t w, h, total, gap, usable, first, min0, min1;
    if (s->busy)
        return;
    s->busy = 1;
    w = lv_obj_get_content_width(s->root);
    h = lv_obj_get_content_height(s->root);
    if (w < 0)
        w = 0;
    if (h < 0)
        h = 0;
    total = s->axis == UI_SPLIT_HORIZONTAL ? w : h;
    gap = gx_lv_px(8);
    if (gap > total)
        gap = total;
    usable = total - gap;
    min0 = gx_lv_px(s->minimum[0]);
    min1 = gx_lv_px(s->minimum[1]);
    if (min0 + min1 > usable) {
        min0 = min0 + min1 ? usable * min0 / (min0 + min1) : 0;
        min1 = usable - min0;
    }
    first = usable * s->position / 1000;
    if (first < min0)
        first = min0;
    if (first > usable - min1)
        first = usable - min1;
    lv_obj_set_pos(s->panes[0], 0, 0);
    if (!s->visible)
        lv_obj_set_size(s->panes[0], w, h);
    else if (s->axis == UI_SPLIT_HORIZONTAL) {
        lv_obj_set_size(s->panes[0], first, h);
        lv_obj_set_pos(s->handle, first, 0);
        lv_obj_set_size(s->handle, gap, h);
        lv_obj_set_pos(s->panes[1], first + gap, 0);
        lv_obj_set_size(s->panes[1], usable - first, h);
    } else {
        lv_obj_set_size(s->panes[0], w, first);
        lv_obj_set_pos(s->handle, 0, first);
        lv_obj_set_size(s->handle, w, gap);
        lv_obj_set_pos(s->panes[1], 0, first + gap);
        lv_obj_set_size(s->panes[1], w, usable - first);
    }
    s->busy = 0;
}
static void split_event(lv_event_t *event) {
    ui_split *s = lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_DELETE)
        free(s);
    else if (code == LV_EVENT_SIZE_CHANGED)
        split_layout(s);
}
static void separator_event(lv_event_t *event) {
    ui_split *s = lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);
    lv_indev_t *input = lv_indev_active();
    lv_point_t point;
    lv_area_t area;
    if (code == LV_EVENT_KEY) {
        uint32_t key = lv_event_get_key(event);
        unsigned p = s->position;
        /* Repeated keys may arrive before the next display refresh. */
        lv_obj_update_layout(s->root);
        int32_t available = (s->axis == UI_SPLIT_HORIZONTAL ? lv_obj_get_width(s->root)
                                                           : lv_obj_get_height(s->root)) - gx_lv_px(8);
        if (available > 0 && key != LV_KEY_HOME && key != LV_KEY_END) {
            int32_t first = s->axis == UI_SPLIT_HORIZONTAL ? lv_obj_get_width(s->panes[0])
                                                          : lv_obj_get_height(s->panes[0]);
            /* Arrow keys move the actual divider immediately, even when the
             * preferred ratio was clamped by a pane minimum. */
            int32_t actual = first * 1000 / available;
            if (actual < (int32_t)p - 2 || actual > (int32_t)p + 2)
                p = actual;
        }
        if (key == LV_KEY_HOME)
            p = 0;
        else if (key == LV_KEY_END)
            p = 1000;
        else if (key == (s->axis == UI_SPLIT_HORIZONTAL ? LV_KEY_LEFT : LV_KEY_UP))
            p = p > 25 ? p - 25 : 0;
        else if (key == (s->axis == UI_SPLIT_HORIZONTAL ? LV_KEY_RIGHT : LV_KEY_DOWN))
            p = p < 975 ? p + 25 : 1000;
        if (key == LV_KEY_HOME || key == LV_KEY_END ||
            key == (s->axis == UI_SPLIT_HORIZONTAL ? LV_KEY_LEFT : LV_KEY_UP) ||
            key == (s->axis == UI_SPLIT_HORIZONTAL ? LV_KEY_RIGHT : LV_KEY_DOWN))
            ui_split_position(s, p);
    } else if (code == LV_EVENT_DOUBLE_CLICKED)
        ui_split_position(s, 500);
    else if (code == LV_EVENT_PRESSED && input &&
             lv_indev_get_type(input) == LV_INDEV_TYPE_POINTER) {
        lv_indev_get_point(input, &point);
        lv_obj_get_coords(s->handle, &area);
        s->grab = s->axis == UI_SPLIT_HORIZONTAL ? point.x - area.x1 : point.y - area.y1;
        s->press_axis = s->axis == UI_SPLIT_HORIZONTAL ? point.x : point.y;
        s->dragging = 1;
        lv_group_focus_obj(s->handle);
    } else if (code == LV_EVENT_PRESSING && s->dragging && input) {
        int32_t coordinate, total, gap = gx_lv_px(8);
        lv_indev_get_point(input, &point);
        if (s->dragging == 1 &&
            (s->axis == UI_SPLIT_HORIZONTAL ? point.x : point.y) == s->press_axis)
            return;
        s->dragging = 2;
        lv_obj_get_content_coords(s->root, &area);
        coordinate = s->axis == UI_SPLIT_HORIZONTAL ? point.x - area.x1 : point.y - area.y1;
        total = s->axis == UI_SPLIT_HORIZONTAL ? lv_area_get_width(&area) : lv_area_get_height(&area);
        if (total > gap) {
            coordinate -= s->grab;
            if (coordinate < 0)
                coordinate = 0;
            if (coordinate > total - gap)
                coordinate = total - gap;
            ui_split_position(s, (unsigned)(coordinate * 1000 / (total - gap)));
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
        s->dragging = 0;
}
ui_split *ui_split_create(lv_obj_t *parent, unsigned axis) {
    ui_split *s;
    unsigned i;
    if (axis > UI_SPLIT_VERTICAL)
        return NULL;
    s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->axis = axis;
    s->position = 500;
    s->minimum[0] = s->minimum[1] = 80;
    s->visible = 1;
    s->root = lv_obj_create(parent);
    lv_obj_remove_style_all(s->root);
    lv_obj_set_scrollable(s->root, false);
    lv_obj_set_click_focusable(s->root, false);
    lv_obj_set_width(s->root, lv_pct(100));
    for (i = 0; i < 2; i++) {
        s->panes[i] = lv_obj_create(s->root);
        lv_obj_remove_style_all(s->panes[i]);
        lv_obj_set_scrollable(s->panes[i], false);
        lv_obj_set_click_focusable(s->panes[i], false);
    }
    s->handle = ui_button(s->root, NULL, UI_TOOL);
    lv_obj_set_style_pad_all(s->handle, 0, 0);
    lv_obj_set_style_border_width(s->handle, 0, 0);
    lv_obj_set_style_radius(s->handle, 0, 0);
    lv_obj_set_style_min_width(s->handle, 0, 0);
    lv_obj_set_style_bg_color(s->handle, ui_colour(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(s->handle, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s->handle, ui_colour(UI_BORDER), LV_STATE_HOVERED);
    lv_obj_set_style_bg_color(s->handle, ui_colour(UI_ACCENT), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_color(s->handle, ui_colour(UI_ACCENT), LV_STATE_PRESSED);
    lv_obj_set_style_outline_width(s->handle, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_scrollable(s->handle, false);
    lv_obj_set_scroll_chain(s->handle, false);
    lv_obj_set_press_lock(s->handle, true);
    lv_obj_add_event_cb(s->handle, separator_event, LV_EVENT_ALL, s);
    lv_obj_add_event_cb(s->root, split_event, LV_EVENT_ALL, s);
    split_layout(s);
    return s;
}
lv_obj_t *ui_split_object(ui_split *s) { return s->root; }
lv_obj_t *ui_split_pane(ui_split *s, unsigned pane) { return pane < 2 ? s->panes[pane] : NULL; }
lv_obj_t *ui_split_handle(ui_split *s) { return s->handle; }
void ui_split_minimum(ui_split *s, unsigned first, unsigned second) {
    s->minimum[0] = first > 8000 ? 8000 : first;
    s->minimum[1] = second > 8000 ? 8000 : second;
    split_layout(s);
}
void ui_split_position(ui_split *s, unsigned position) {
    position = position > 1000 ? 1000 : position;
    if (s->position == position)
        return;
    s->position = position;
    split_layout(s);
    lv_obj_send_event(s->root, LV_EVENT_VALUE_CHANGED, NULL);
}
unsigned ui_split_get_position(ui_split *s) { return s->position; }
int ui_split_second_visible(ui_split *s) { return s->visible; }
void ui_split_show_second(ui_split *s, int show) {
    lv_obj_t *focused, *ancestor;
    show = !!show;
    if (s->visible == (unsigned)show)
        return;
    s->visible = show;
    s->dragging = 0;
    focused = gx_lv_group() ? lv_group_get_focused(gx_lv_group()) : NULL;
    if (show) {
        lv_obj_set_hidden(s->panes[1], false);
        lv_obj_set_hidden(s->handle, false);
    } else {
        /* Hidden descendants must not keep consuming editor/table key input. */
        ancestor = focused;
        while (ancestor && ancestor != s->panes[1])
            ancestor = lv_obj_get_parent(ancestor);
        if (focused && (focused == s->handle || ancestor == s->panes[1]))
            ui_focus_clear(gx_lv_group());
        lv_obj_set_hidden(s->panes[1], true);
        lv_obj_set_hidden(s->handle, true);
    }
    split_layout(s);
    lv_obj_send_event(s->root, LV_EVENT_VALUE_CHANGED, NULL);
}
void ui_split_density(ui_split *s) { split_layout(s); }
