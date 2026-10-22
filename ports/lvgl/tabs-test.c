/* SPDX-License-Identifier: MIT */
#include "ui-tabs.c"
#include <assert.h>
#include <stdio.h>
static unsigned chosen, close_requested;
static ui_tabs *destroy_on_close, *destroy_on_select;
static ui_tabs *rebind_on_close, *delete_on_select;
static const struct ui_tab_spec rebound[] = {{20, "Kept", 0, 1}, {30, "Fixed", 0, 0}};
static lv_indev_data_t pointer_sample;
static void pointer_read(lv_indev_t *pointer, lv_indev_data_t *data) {
    (void)pointer;
    *data = pointer_sample;
}
static void move_pointer(lv_indev_t *pointer, int32_t x, int32_t y) {
    memset(&pointer_sample, 0, sizeof(pointer_sample));
    pointer_sample.point.x = x;
    pointer_sample.point.y = y;
    lv_indev_read(pointer);
}
static void click_pointer(lv_indev_t *pointer, int32_t x, int32_t y) {
    move_pointer(pointer, x, y);
    pointer_sample.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(pointer);
    pointer_sample.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(pointer);
}
static void changed(unsigned id, int close, void *user) {
    assert(user == &chosen);
    chosen = id;
    close_requested = close;
    if (!close && delete_on_select) {
        ui_tabs *tabs = delete_on_select;
        delete_on_select = NULL;
        lv_obj_delete(tabs->entries[1].button);
    }
    if (close && rebind_on_close) {
        ui_tabs *tabs = rebind_on_close;
        rebind_on_close = NULL;
        assert(ui_tabs_bind(tabs, rebound, 2, 20));
    }
    if (!close && destroy_on_select) {
        lv_obj_delete(ui_tabs_object(destroy_on_select));
        destroy_on_select = NULL;
    }
    if (close && destroy_on_close) {
        lv_obj_delete(ui_tabs_object(destroy_on_close));
        destroy_on_close = NULL;
    }
}
static void delete_on_focus(lv_event_t *event) {
    lv_obj_delete(ui_tabs_object(lv_event_get_user_data(event)));
}
static void single_line_caption(struct tab_entry *entry, int ellipsized) {
    lv_area_t row, label;
    const char *text = lv_label_get_text(entry->label);
    size_t length = strlen(text);
    assert(lv_obj_get_height(entry->label) ==
           lv_font_get_line_height(lv_obj_get_style_text_font(entry->label, LV_PART_MAIN)));
    lv_obj_get_coords(entry->button, &row);
    lv_obj_get_coords(entry->label, &label);
    assert(label.y1 >= row.y1 && label.y2 <= row.y2);
    if (ellipsized)
        assert(length >= 3 && !strcmp(text + length - 3, "..."));
}
int main(void) {
    struct ui_tab_spec specs[] = {{10, "Привіт.c", 0, 1}, {20, "Notes", 1, 1}, {30, "Fixed", 0, 0}};
    struct ui_tab_spec invalid[] = {{10, "Duplicate", 0, 1}, {10, "Duplicate", 0, 1}};
    ui_tabs *tabs;
    lv_obj_t *button;
    lv_indev_t *pointer, *keypad;
    unsigned n;
    char copied[32] = "Mutable label";
    char *long_label = malloc(2050);
    assert(long_label);
    memset(long_label,'W',2048);long_label[2048] = 0;
    assert(!gx_lv_open("Tabs test", 600, 300));
    ui_screen();
    pointer = lv_indev_create();
    assert(pointer);
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_mode(pointer, LV_INDEV_MODE_EVENT);
    lv_indev_set_read_cb(pointer, pointer_read);
    keypad = lv_indev_create();
    assert(keypad);
    lv_indev_set_type(keypad, LV_INDEV_TYPE_KEYPAD);
    tabs = ui_tabs_create(lv_screen_active(), changed, &chosen);
    assert(tabs && ui_tabs_bind(tabs, specs, 3, 10));
    lv_obj_update_layout(lv_screen_active());
    assert(tabs->count == 3 && ui_tabs_selected(tabs) == 10);
    assert(!strcmp(tabs->entries[1].text, "Notes"));
    assert(lv_obj_get_style_bg_opa(tabs->entries[1].dirty_mark, 0) == LV_OPA_COVER);
    assert(lv_obj_get_style_opa(tabs->entries[0].close, 0) == LV_OPA_TRANSP);
    assert(lv_obj_get_height(tabs->root) == gx_lv_px(32));
    assert(lv_obj_get_height(tabs->entries[0].button) == gx_lv_px(30));
    assert(lv_obj_get_child(tabs->root, 0) == tabs->entries[0].button);
    assert(lv_obj_get_child(tabs->entries[0].button, 1) == tabs->entries[0].close);
    button = tabs->entries[1].button;
    assert(!ui_tabs_bind(tabs, invalid, 2, 10));
    assert(tabs->count == 3 && tabs->entries[1].button == button);
    assert(!ui_tabs_bind(tabs, specs, 3, 99));
    specs[1].dirty = 0;
    {
        int32_t before = lv_obj_get_width(button);
    assert(ui_tabs_bind(tabs, specs, 3, 10));
    assert(tabs->entries[1].button == button && !strcmp(tabs->entries[1].text, "Notes"));
        assert(lv_obj_get_width(button) == before);
        assert(lv_obj_get_style_bg_opa(tabs->entries[1].dirty_mark, 0) == LV_OPA_TRANSP);
    }
    lv_obj_send_event(button, LV_EVENT_CLICKED, NULL);
    assert(chosen == 20 && !close_requested && ui_tabs_selected(tabs) == 20);
    /* Real pointer hover must survive crossing from the tab into its close
     * child, and revealing the control must never reflow the strip. */
    {
        lv_area_t row, close, label;
        int32_t before = lv_obj_get_width(tabs->entries[0].button);
        lv_obj_get_coords(tabs->entries[0].button, &row);
        lv_obj_get_coords(tabs->entries[0].close, &close);
        lv_obj_get_coords(tabs->entries[0].label, &label);
        assert(close.x2 < label.x1);
        move_pointer(pointer, row.x2 - gx_lv_px(12), (row.y1 + row.y2) / 2);
        assert(lv_obj_get_style_opa(tabs->entries[0].close, 0) == LV_OPA_COVER);
        move_pointer(pointer, (close.x1 + close.x2) / 2, (close.y1 + close.y2) / 2);
        assert(lv_obj_get_style_opa(tabs->entries[0].close, 0) == LV_OPA_COVER);
        lv_obj_update_layout(lv_screen_active());
        assert(lv_obj_get_width(tabs->entries[0].button) == before);
        click_pointer(pointer, (close.x1 + close.x2) / 2, (close.y1 + close.y2) / 2);
        assert(ui_tabs_selected(tabs) == 20);
        move_pointer(pointer, row.x1, row.y2 + gx_lv_px(60));
        assert(lv_obj_get_style_opa(tabs->entries[0].close, 0) == LV_OPA_TRANSP);
    }
    assert(chosen == 10 && close_requested && tabs->count == 3);
    /* Nonclosable tabs preserve the same slot but it acts as tab background. */
    {
        lv_area_t close;
        lv_obj_get_coords(tabs->entries[2].close, &close);
        click_pointer(pointer, (close.x1 + close.x2) / 2, (close.y1 + close.y2) / 2);
        assert(chosen == 30 && !close_requested && ui_tabs_selected(tabs) == 30);
        assert(lv_obj_get_style_opa(tabs->entries[2].close, 0) == LV_OPA_TRANSP);
        move_pointer(pointer, close.x1, close.y2 + gx_lv_px(60));
    }
    lv_obj_send_event(button, LV_EVENT_FOCUSED, keypad);
    assert(lv_obj_get_style_opa(tabs->entries[1].close, 0) == LV_OPA_COVER);
    lv_obj_send_event(button, LV_EVENT_DEFOCUSED, keypad);
    assert(lv_obj_get_style_opa(tabs->entries[1].close, 0) == LV_OPA_TRANSP);
    {
        uint32_t key = LV_KEY_END;
        lv_obj_send_event(button, LV_EVENT_KEY, &key);
    }
    assert(chosen == 30 && ui_tabs_selected(tabs) == 30);
    /* Renaming and dirty-state changes preserve keyboard focus and widgets. */
    lv_group_focus_obj(button);
    specs[1].text = copied;specs[1].dirty = 1;
    assert(ui_tabs_bind(tabs, specs, 3, 20));
    strcpy(copied,"Changed by caller");
    assert(!strcmp(tabs->entries[1].text,"Mutable label"));
    assert(lv_obj_get_style_bg_opa(tabs->entries[1].dirty_mark, 0) == LV_OPA_COVER);
    assert(tabs->entries[1].button == button);
    assert(lv_group_get_focused(gx_lv_group()) == button);
    /* Long names remain bounded without consuming the separate dirty marker. */
    specs[1].text = long_label;
    assert(ui_tabs_bind(tabs, specs, 3, 20));
    lv_obj_update_layout(lv_screen_active());
    assert(lv_obj_get_width(tabs->entries[1].label) <= gx_lv_px(180));
    single_line_caption(&tabs->entries[1], 1);
    assert(lv_obj_get_width(tabs->entries[1].dirty_mark) == gx_lv_px(6));
    assert(lv_obj_get_style_bg_opa(tabs->entries[1].dirty_mark, 0) == LV_OPA_COVER);
    long_label[2048] = 'W';long_label[2049] = 0;
    assert(!ui_tabs_bind(tabs, specs, 3, 20));
    assert(tabs->entries[1].button == button);
    long_label[2048] = 0;
    specs[1].text = "Notes";specs[1].dirty = 0;
    ui_tabs_density(tabs);
    assert(ui_tabs_bind(tabs, NULL, 0, 0));
    assert(tabs->count == 0 && ui_tabs_selected(tabs) == 0);
    assert(ui_tabs_bind(tabs, specs, 3, 20));
    rebind_on_close = tabs;
    lv_obj_send_event(tabs->entries[0].close, LV_EVENT_CLICKED, NULL);
    assert(!rebind_on_close && tabs->count == 2 && ui_tabs_selected(tabs) == 20);
    assert(ui_tabs_bind(tabs, specs, 3, 20));
    delete_on_select = tabs;
    lv_obj_send_event(tabs->entries[1].button, LV_EVENT_CLICKED, NULL);
    assert(!delete_on_select && !tabs->entries[1].button);
    assert(ui_tabs_bind(tabs, specs, 3, 20));
    assert(tabs->entries[1].button);
    /* Selection reveals offscreen tabs even when the strip is very narrow. */
    {
        struct ui_tab_spec many[UI_TABS_LIMIT];
        lv_area_t row, strip;
        for (n = 0; n < UI_TABS_LIMIT; n++) {
            many[n].id = 100 + n;
            many[n].text = "Довга назва документа — програмування та перевірка.c";
            many[n].dirty = n & 1;
            many[n].closable = 1;
        }
        gx_lv_width(tabs->root, 260);
        assert(ui_tabs_bind(tabs, many, UI_TABS_LIMIT, 100));
        for (n = 0; n < UI_TABS_LIMIT; n++)
            single_line_caption(&tabs->entries[n], 1);
        assert(lv_obj_get_scroll_x(tabs->root) == 0);
        ui_tabs_select(tabs, 100 + UI_TABS_LIMIT - 1);
        assert(lv_obj_get_scroll_x(tabs->root) > 0);
        lv_obj_get_coords(tabs->root, &strip);
        lv_obj_get_coords(tabs->entries[UI_TABS_LIMIT - 1].button, &row);
        assert(row.x1 >= strip.x1 && row.x2 <= strip.x2);
        assert(lv_obj_get_style_bg_opa(tabs->entries[UI_TABS_LIMIT - 1].button, 0) == LV_OPA_COVER);
        assert(lv_obj_get_style_bg_opa(tabs->entries[0].button, 0) == LV_OPA_TRANSP);
    }
    lv_obj_delete(ui_tabs_object(tabs));
    for (n = 0; n < 20; n++) {
        tabs = ui_tabs_create(lv_screen_active(), changed, &chosen);
        assert(tabs && ui_tabs_bind(tabs, specs, 3, 10));
        destroy_on_close = tabs;
        lv_obj_send_event(tabs->entries[0].close, LV_EVENT_CLICKED, NULL);
        assert(!destroy_on_close);
    }
    tabs = ui_tabs_create(lv_screen_active(), changed, &chosen);
    assert(tabs && ui_tabs_bind(tabs,specs,3,10));
    destroy_on_select = tabs;
    lv_obj_send_event(tabs->entries[1].button,LV_EVENT_CLICKED,NULL);
    assert(!destroy_on_select);
    tabs = ui_tabs_create(lv_screen_active(), changed, &chosen);
    assert(tabs && ui_tabs_bind(tabs, specs, 3, 10));
    lv_obj_add_event_cb(tabs->entries[1].button, delete_on_focus, LV_EVENT_FOCUSED, tabs);
    {
        uint32_t key = LV_KEY_RIGHT;
        lv_obj_send_event(tabs->entries[0].button, LV_EVENT_KEY, &key);
    }
    lv_indev_delete(pointer);
    lv_indev_delete(keypad);
    free(long_label);
    gx_lv_close();
    puts("TABS passed compact integrated tabs, left hover/focus close controls, pointer transfer, stable geometry, background/nonclosable close policy, copied Unicode labels, dirty indicators, overflow navigation, invalid bindings, retained focus, callback rebind/tab deletion/root teardown");
    return 0;
}
