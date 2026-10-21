/* SPDX-License-Identifier: MIT */
#include "ui-tabs.c"
#include <assert.h>
#include <stdio.h>
static unsigned chosen, close_requested;
static ui_tabs *destroy_on_close, *destroy_on_select;
static void changed(unsigned id, int close, void *user) {
    assert(user == &chosen);
    chosen = id;
    close_requested = close;
    if (!close && destroy_on_select) {
        lv_obj_delete(ui_tabs_object(destroy_on_select));
        destroy_on_select = NULL;
    }
    if (close && destroy_on_close) {
        lv_obj_delete(ui_tabs_object(destroy_on_close));
        destroy_on_close = NULL;
    }
}
int main(void) {
    struct ui_tab_spec specs[] = {{10, "Привіт.c", 0, 1}, {20, "Notes", 1, 1}, {30, "Fixed", 0, 0}};
    struct ui_tab_spec invalid[] = {{10, "Duplicate", 0, 1}, {10, "Duplicate", 0, 1}};
    ui_tabs *tabs;
    lv_obj_t *button;
    unsigned n;
    char copied[32] = "Mutable label";
    char *long_label = malloc(2050);
    assert(long_label);
    memset(long_label,'W',2048);long_label[2048] = 0;
    assert(!gx_lv_open("Tabs test", 600, 300));
    ui_screen();
    tabs = ui_tabs_create(lv_screen_active(), changed, &chosen);
    assert(tabs && ui_tabs_bind(tabs, specs, 3, 10));
    lv_obj_update_layout(lv_screen_active());
    assert(tabs->count == 3 && ui_tabs_selected(tabs) == 10);
    assert(!strcmp(tabs->entries[1].text, "* Notes"));
    button = tabs->entries[1].button;
    assert(!ui_tabs_bind(tabs, invalid, 2, 10));
    assert(tabs->count == 3 && tabs->entries[1].button == button);
    assert(!ui_tabs_bind(tabs, specs, 3, 99));
    specs[1].dirty = 0;
    assert(ui_tabs_bind(tabs, specs, 3, 10));
    assert(tabs->entries[1].button == button && !strcmp(tabs->entries[1].text, "Notes"));
    lv_obj_send_event(button, LV_EVENT_CLICKED, NULL);
    assert(chosen == 20 && !close_requested && ui_tabs_selected(tabs) == 20);
    lv_obj_send_event(tabs->entries[0].close, LV_EVENT_CLICKED, NULL);
    assert(chosen == 10 && close_requested && tabs->count == 3);
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
    assert(!strcmp(tabs->entries[1].text,"* Mutable label"));
    assert(tabs->entries[1].button == button);
    assert(lv_group_get_focused(gx_lv_group()) == button);
    /* Long names remain bounded and keep their dirty marker visible. */
    specs[1].text = long_label;
    assert(ui_tabs_bind(tabs, specs, 3, 20));
    lv_obj_update_layout(lv_screen_active());
    assert(lv_obj_get_width(tabs->entries[1].label) <= gx_lv_px(180));
    assert(!strncmp(lv_label_get_text(tabs->entries[1].label),"* ",2));
    long_label[2048] = 'W';long_label[2049] = 0;
    assert(!ui_tabs_bind(tabs, specs, 3, 20));
    assert(tabs->entries[1].button == button);
    long_label[2048] = 0;
    specs[1].text = "Notes";specs[1].dirty = 0;
    ui_tabs_density(tabs);
    assert(ui_tabs_bind(tabs, NULL, 0, 0));
    assert(tabs->count == 0 && ui_tabs_selected(tabs) == 0);
    assert(ui_tabs_bind(tabs, specs, 3, 20));
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
    free(long_label);
    gx_lv_close();
    puts("TABS passed copied Unicode labels, dirty updates, stable widgets, invalid bindings, selection, keyboard, close requests and long-label bounds, retained focus, empty selection and callback teardown");
    return 0;
}
