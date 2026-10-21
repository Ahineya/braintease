/* SPDX-License-Identifier: MIT */
/* Independent application-component invariants, also run under UBSan. */
#include "ui-app.c"
#include <assert.h>
#include <stdio.h>
static unsigned calls, last, changes;
static void execute(unsigned id, void *user) {
    assert(user == &calls);
    calls++;
    last = id;
}
static void changed(lv_event_t *event) {
    (void)event;
    changes++;
}
static void action_checks(void) {
    static const struct ui_action_spec specs[] = {
        {1, "Run", "Ctrl+R", "play", 21, 1, 1},
        {2, "Save", "Ctrl+Shift+S", "save", 22, 3, 0}
    };
    static const unsigned ids[] = {1, 2};
    struct ui_action_spec bad[2];
    ui_actions *a = ui_actions_create(specs, 2, execute, &calls);
    lv_obj_t *container = ui_panel(lv_screen_active()), *one, *two, *save;
    lv_obj_t *menu;
    struct ui_menu_item item;
    assert(a);
    one = ui_actions_button(a, container, 1, UI_BUTTON);
    two = ui_actions_button(a, container, 1, UI_PRIMARY);
    save = ui_actions_button(a, container, 2, UI_BUTTON);
    assert(one && two && save);
    assert(lv_obj_has_state(save, LV_STATE_DISABLED));
    assert(ui_actions_key(a, 21, 0, 1) && !calls);
    assert(!ui_actions_key(a, 21, 1, 0));
    assert(ui_actions_key(a, 21, 1, 1) && calls == 1 && last == 1);
    lv_obj_send_event(two, LV_EVENT_CLICKED, NULL);
    assert(calls == 2);
    ui_actions_enable(a, 1, 0);
    assert(lv_obj_has_state(one, LV_STATE_DISABLED) && lv_obj_has_state(two, LV_STATE_DISABLED));
    lv_obj_send_event(one, LV_EVENT_CLICKED, NULL);
    assert(ui_actions_key(a, 21, 1, 1) && calls == 2);
    assert(!ui_actions_invoke(a, 2));
    assert(ui_actions_item(a, 1, &item) && item.disabled && !strcmp(item.shortcut, "Ctrl+R"));
    ui_actions_enable(a, 2, 1);
    assert(!lv_obj_has_state(save, LV_STATE_DISABLED));
    assert(!ui_actions_key(a, 22, 1, 1));
    assert(ui_actions_key(a, 22, 1, 3) && calls == 3 && last == 2);
    ui_message("Modal", "Background commands cannot run.");
    assert(ui_actions_key(a, 22, 1, 3) && !ui_actions_invoke(a, 2) && calls == 3);
    assert(!ui_overlay_close_for(a) && ui_overlay_active());
    ui_overlay_close();
    ui_actions_enable(a, 1, 1);
    lv_obj_update_layout(lv_screen_active());
    assert(ui_actions_menu(a, one, ids, 2));
    menu = lv_obj_get_child(lv_obj_get_child(lv_layer_top(), 0), 0);
    /* State changed while a menu was open still cannot execute a stale item. */
    ui_actions_enable(a, 1, 0);
    lv_obj_send_event(lv_obj_get_child(menu, 0), LV_EVENT_CLICKED, NULL);
    assert(!ui_overlay_active() && calls == 3);
    assert(ui_actions_menu(a, one, ids, 2));
    menu = lv_obj_get_child(lv_obj_get_child(lv_layer_top(), 0), 0);
    lv_obj_send_event(lv_obj_get_child(menu, 1), LV_EVENT_CLICKED, NULL);
    assert(!ui_overlay_active() && calls == 4 && last == 2);
    lv_obj_delete(two);
    ui_actions_enable(a, 1, 1);
    assert(ui_actions_menu(a, one, ids, 2));
    ui_actions_delete(a);
    assert(!ui_overlay_active());
    lv_obj_send_event(one, LV_EVENT_CLICKED, NULL);
    assert(calls == 4 && lv_obj_has_state(one, LV_STATE_DISABLED));
    lv_obj_delete(container);
    /* The opposite ownership order must also be safe. */
    a = ui_actions_create(specs, 2, execute, &calls);
    container = ui_panel(lv_screen_active());
    ui_actions_button(a, container, 1, UI_BUTTON);
    lv_obj_delete(container);
    assert(!a->bindings);
    ui_actions_delete(a);
    memcpy(bad, specs, sizeof(bad));
    bad[1].id = bad[0].id;
    assert(!ui_actions_create(bad, 2, execute, &calls));
    memcpy(bad, specs, sizeof(bad));
    bad[1].key = bad[0].key;
    bad[1].modifiers = bad[0].modifiers;
    assert(!ui_actions_create(bad, 2, execute, &calls));
    assert(!ui_actions_create(specs, 0, execute, &calls));
}
static void settled(ui_split *s) {
    int32_t total, a, b, gap;
    lv_obj_update_layout(s->root);
    total = s->axis == UI_SPLIT_HORIZONTAL ? lv_obj_get_width(s->root) : lv_obj_get_height(s->root);
    a = s->axis == UI_SPLIT_HORIZONTAL ? lv_obj_get_width(s->panes[0]) : lv_obj_get_height(s->panes[0]);
    b = s->axis == UI_SPLIT_HORIZONTAL ? lv_obj_get_width(s->panes[1]) : lv_obj_get_height(s->panes[1]);
    gap = s->axis == UI_SPLIT_HORIZONTAL ? lv_obj_get_width(s->handle) : lv_obj_get_height(s->handle);
    assert(a >= 0 && b >= 0 && gap >= 0);
    if (s->visible)
        assert(a + b + gap == total);
    else
        assert(a == total && lv_obj_is_hidden(s->handle) && lv_obj_is_hidden(s->panes[1]));
}
static void split_checks(void) {
    unsigned axis, scale, i;
    for (axis = 0; axis < 2; axis++) {
        ui_split *s = ui_split_create(lv_screen_active(), axis);
        lv_obj_t *button = ui_button(s->panes[1], "Preserved contents", UI_BUTTON);
        assert(s);
        lv_obj_add_event_cb(s->root, changed, LV_EVENT_VALUE_CHANGED, NULL);
        ui_split_minimum(s, 120, 100);
        ui_split_position(s, 700);
        for (scale = 1; scale <= 2; scale++) {
            unsigned before = changes;
            gx_lv_apply_density(scale);
            gx_lv_size(s->root, 700, 400);
            ui_split_density(s);
            settled(s);
            assert(ui_split_get_position(s) == 700 && changes == before);
            /* A small viewport clamps geometry, never the preferred ratio. */
            gx_lv_size(s->root, 90, 70);
            settled(s);
            assert(ui_split_get_position(s) == 700);
            gx_lv_size(s->root, 700, 400);
            settled(s);
            lv_group_focus_obj(button);
            ui_split_show_second(s, 0);
            assert(!lv_group_get_focused(gx_lv_group()));
            settled(s);
            ui_split_show_second(s, 1);
            settled(s);
            assert(lv_obj_get_parent(button) == s->panes[1]);
            lv_group_focus_obj(s->handle);
            lv_group_send_data(gx_lv_group(), axis ? LV_KEY_DOWN : LV_KEY_RIGHT);
            assert(ui_split_get_position(s) == 725);
            lv_group_send_data(gx_lv_group(), axis ? LV_KEY_UP : LV_KEY_LEFT);
            assert(ui_split_get_position(s) == 700);
            lv_group_send_data(gx_lv_group(), LV_KEY_HOME);
            settled(s);
            assert(ui_split_get_position(s) == 0);
            lv_group_send_data(gx_lv_group(), LV_KEY_END);
            settled(s);
            assert(ui_split_get_position(s) == 1000);
            {
                int32_t before = axis ? lv_obj_get_height(s->panes[0]) : lv_obj_get_width(s->panes[0]);
                lv_group_send_data(gx_lv_group(), axis ? LV_KEY_UP : LV_KEY_LEFT);
                settled(s);
                assert((axis ? lv_obj_get_height(s->panes[0]) : lv_obj_get_width(s->panes[0])) < before);
            }
            lv_obj_send_event(s->handle, LV_EVENT_DOUBLE_CLICKED, NULL);
            assert(ui_split_get_position(s) == 500);
            for (i = 0; i < 20; i++) {
                gx_lv_size(s->root, 8 + i * 35, 8 + i * 20);
                settled(s);
            }
            ui_split_position(s, 700);
        }
        lv_obj_delete(s->root);
    }
    assert(!ui_split_create(lv_screen_active(), 2));
}
int main(void) {
    assert(!gx_lv_open("Application component checks", 800, 560));
    ui_screen();
    action_checks();
    split_checks();
    gx_lv_close();
    puts("APP_COMPONENT passed: commands, ownership, modal isolation, split geometry and density");
    return 0;
}
