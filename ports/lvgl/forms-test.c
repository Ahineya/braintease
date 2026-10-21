/* SPDX-License-Identifier: MIT */
/* Component tests use private state only for ownership and geometry invariants. */
#include "ui-forms.c"
#include <assert.h>
static unsigned choice_changes, accepted, cancelled, browsed, cancelled_jobs;
static int reject_path;
static char received[UI_PATH_LIMIT];
static void choice_changed(lv_event_t *event) {
    (void)event;
    choice_changes++;
}
static int path_result(ui_path *p, const char *path, void *user) {
    assert(user == &accepted);
    if (!path) {
        cancelled++;
        return 0;
    }
    accepted++;
    assert(strlen(path) < sizeof(received));
    strcpy(received, path);
    if (reject_path) {
        ui_path_error(p, "Test application refused the file.");
        return 0;
    }
    return 1;
}
static void browse(ui_path *p, void *user) {
    assert(user == &browsed);
    browsed++;
    ui_path_set(p, "existing.txt");
}
static void cancel_job(ui_progress *p, void *user) {
    assert(user == &cancelled_jobs && ui_progress_cancelled(p));
    cancelled_jobs++;
}
static void settled_path(ui_path *p) {
    unsigned steps = 0;
    char before[UI_PATH_LIMIT], after[UI_PATH_LIMIT];
    assert(getcwd(before, sizeof(before)));
    while (ui_path_loading(p)) {
        assert(++steps < 10000);
        path_scan_tick(p->scan_timer);
        assert(getcwd(after, sizeof(after)) && !strcmp(before, after));
    }
    lv_obj_update_layout(p->body);
}
static void choice_checks(void) {
    char mutable[] = "First";
    const char *labels[] = {mutable, "Second", "Third"};
    const char **many = calloc(UI_CHOICE_LIMIT, sizeof(*many));
    ui_choice *c = ui_choice_create(lv_screen_active(), labels, 3, 1);
    lv_obj_t *focused, *frame;
    unsigned i, before;
    assert(c && many);
    gx_lv_width(c->root, 280);
    assert(lv_obj_get_child_count(c->root) == 2);
    mutable[0] = 'X';
    assert(!strcmp(c->labels[0], "First"));
    lv_obj_add_event_cb(c->root, choice_changed, LV_EVENT_VALUE_CHANGED, NULL);
    ui_choice_select(c, 1);
    assert(!choice_changes);
    ui_choice_select(c, 2);
    assert(choice_changes == 1);
    lv_group_focus_obj(c->root);
    lv_group_send_data(gx_lv_group(), LV_KEY_HOME);
    assert(ui_choice_selected(c) == 0 && choice_changes == 2);
    lv_group_send_data(gx_lv_group(), LV_KEY_DOWN);
    assert(ui_choice_selected(c) == 1 && choice_changes == 3);
    for (i = 0; i < UI_CHOICE_LIMIT; i++)
        many[i] = i == 64 ? "Last column" : "Column";
    assert(ui_choice_bind(c, many, UI_CHOICE_LIMIT, 64));
    assert(ui_choice_selected(c) == 64 && choice_changes == 3);
    assert(!ui_choice_bind(c, many, UI_CHOICE_LIMIT + 1, 0));
    assert(!ui_choice_bind(c, labels, 3, 3));
    assert(c->count == UI_CHOICE_LIMIT && c->selected == 64);
    lv_obj_update_layout(lv_screen_active());
    lv_obj_send_event(c->root, LV_EVENT_CLICKED, NULL);
    assert(ui_overlay_active());
    focused = lv_group_get_focused(gx_lv_group());
    frame = lv_obj_get_parent(focused);
    assert(lv_obj_get_child_count(frame) == UI_CHOICE_LIMIT);
    assert(focused == lv_obj_get_child(frame, 64));
    lv_group_send_data(gx_lv_group(), LV_KEY_UP);
    assert(c->selected == 64);
    assert(ui_overlay_key(41, 1, 0));
    assert(!ui_overlay_active() && c->selected == 64);
    lv_obj_send_event(c->root, LV_EVENT_CLICKED, NULL);
    lv_group_send_data(gx_lv_group(), LV_KEY_UP);
    lv_obj_send_event(lv_group_get_focused(gx_lv_group()), LV_EVENT_CLICKED, NULL);
    assert(!ui_overlay_active() && c->selected == 63 && choice_changes == 4);
    before = choice_changes;
    lv_obj_add_state(c->root, LV_STATE_DISABLED);
    lv_obj_send_event(c->root, LV_EVENT_CLICKED, NULL);
    assert(!ui_overlay_active() && choice_changes == before);
    lv_obj_remove_state(c->root, LV_STATE_DISABLED);
    lv_obj_send_event(c->root, LV_EVENT_CLICKED, NULL);
    assert(ui_overlay_active());
    /* Rebinding while open invalidates the popup's old indices safely. */
    assert(ui_choice_bind(c, labels, 3, 1));
    assert(!ui_overlay_active() && c->selected == 1);
    for (i = 1; i <= 2; i++) {
        gx_lv_apply_density(i);
        ui_choice_density(c);
        lv_obj_update_layout(c->root);
        assert(lv_obj_get_height(c->root) == gx_lv_px(UI_CONTROL));
    }
    lv_obj_send_event(c->root, LV_EVENT_CLICKED, NULL);
    lv_obj_delete(c->root);
    assert(!ui_overlay_active());
    assert(!ui_choice_create(lv_screen_active(), labels, 0, 0));
    free(many);
}
static void path_checks(void) {
    ui_path *p;
    char *long_path = malloc(UI_PATH_LIMIT + 100);
    char absolute[UI_PATH_LIMIT];
    unsigned before, i;
    assert(long_path);
    memset(long_path, 'x', UI_PATH_LIMIT + 99);
    long_path[UI_PATH_LIMIT + 99] = 0;
    assert(getcwd(absolute, sizeof(absolute)));
    strcat(absolute, "/existing.txt");
    assert(!ui_path_open("Invalid", 99, "", path_result, &accepted));
    p = ui_path_open("Open file", UI_PATH_OPEN, "", path_result, &accepted);
    assert(p && !ui_path_open("Second", UI_PATH_OPEN, "", path_result, &accepted));
    settled_path(p);
    ui_path_submit(p);
    assert(ui_overlay_active() && p->has_error && !accepted);
    ui_path_set(p, long_path);
    assert(strlen(lv_textarea_get_text(p->input)) == UI_PATH_LIMIT + 99);
    ui_path_submit(p);
    assert(ui_overlay_active() && p->has_error && !accepted);
    ui_path_set(p, ".");
    ui_path_submit(p);
    assert(ui_overlay_active() && ui_path_loading(p) && !accepted);
    settled_path(p);
    assert(ui_table_focused(ui_path_list(p)));
    ui_path_set(p, "missing.txt");
    ui_path_submit(p);
    assert(ui_overlay_active() && p->has_error && !accepted);
    assert(ui_path_browse(p, browse, &browsed));
    lv_obj_add_state(p->browse, LV_STATE_DISABLED);
    lv_obj_send_event(p->browse, LV_EVENT_CLICKED, NULL);
    assert(!browsed);
    lv_obj_remove_state(p->browse, LV_STATE_DISABLED);
    lv_obj_send_event(p->browse, LV_EVENT_CLICKED, NULL);
    assert(browsed == 1 && !strcmp(lv_textarea_get_text(p->input), "existing.txt"));
    reject_path = 1;
    ui_path_submit(p);
    assert(ui_overlay_active() && accepted == 1 && p->has_error);
    assert(!strcmp(lv_label_get_text(p->status), "Test application refused the file."));
    reject_path = 0;
    ui_path_set(p, absolute);
    for (i = 1; i <= 2; i++) {
        gx_lv_apply_density(i);
        ui_path_density(p);
        ui_data_resize();
        lv_obj_update_layout(lv_layer_top());
        assert(lv_obj_get_height(p->input) == gx_lv_px(UI_CONTROL));
    }
    ui_path_submit(p);
    assert(!ui_overlay_active() && accepted == 2 && !strcmp(received, absolute) && !cancelled);
    p = ui_path_open("Save file", UI_PATH_SAVE, "missing-parent/new.txt", path_result, &accepted);
    settled_path(p);
    ui_path_submit(p);
    assert(ui_overlay_active() && p->has_error && accepted == 2);
    ui_path_set(p, "new.txt");
    ui_path_submit(p);
    assert(!ui_overlay_active() && accepted == 3 && !strcmp(strrchr(received, '/') + 1, "new.txt"));
    p = ui_path_open("Save file", UI_PATH_SAVE, "existing.txt", path_result, &accepted);
    settled_path(p);
    ui_path_submit(p);
    assert(ui_overlay_active() && p->confirming && accepted == 3);
    assert(!strcmp(lv_label_get_text(lv_obj_get_child(p->accept, 0)), "Replace"));
    ui_path_set(p, "other.txt");
    assert(!p->confirming && !p->pending);
    ui_path_submit(p);
    assert(ui_overlay_active() && p->confirming && accepted == 3);
    ui_path_submit(p);
    assert(!ui_overlay_active() && accepted == 4 && !strcmp(strrchr(received, '/') + 1, "other.txt"));
    p = ui_path_open("Cancel", UI_PATH_OPEN, "existing.txt", path_result, &accepted);
    before = cancelled;
    ui_overlay_key(41, 1, 0);
    assert(!ui_overlay_active() && cancelled == before + 1);
    p = ui_path_open("Window closing", UI_PATH_OPEN, "existing.txt", path_result, &accepted);
    before = cancelled;
    ui_overlay_close();
    assert(cancelled == before + 1);
    p = ui_path_open("Cancel API", UI_PATH_SAVE, "existing.txt", path_result, &accepted);
    ui_path_cancel(p);
    assert(!ui_overlay_active() && cancelled == before + 2);
    free(long_path);
}
static unsigned path_find(ui_path *p, const char *name) {
    unsigned i;
    for (i = 0; i < p->listing.count; i++)
        if (!strcmp(p->listing.entries[i].name, name))
            return i;
    return BROWSER_LIMIT;
}
static void fixture_file(const char *name) {
    FILE *file = fopen(name, "w");
    assert(file);
    assert(fputs("File dialog fixture.\n", file) >= 0);
    assert(!fclose(file));
}
static void file_dialog_checks(void) {
    ui_path *p;
    lv_obj_t *frame;
    lv_area_t bounds;
    char cwd[UI_PATH_LIMIT], child[UI_PATH_LIMIT], full[UI_PATH_LIMIT];
    char text[40];
    unsigned i, scale, size, row, before;
    int32_t old_width = lv_display_get_horizontal_resolution(lv_display_get_default());
    int32_t old_height = lv_display_get_vertical_resolution(lv_display_get_default());
    assert(getcwd(cwd, sizeof(cwd)));
    assert(!browser_join(cwd, "folder", child));
    if (mkdir(child, 0700))
        assert(errno == EEXIST);
    fixture_file("folder/inside.txt");
    fixture_file(".hidden-dialog.txt");
    fixture_file("Київ.txt");
    for (i = 0; i < 40; i++) {
        snprintf(text, sizeof(text), "list-%02u.txt", i);
        fixture_file(text);
    }
    p = ui_path_open("Files", UI_PATH_OPEN, child, path_result, &accepted);
    assert(p && ui_path_loading(p));
    assert(!strcmp(lv_textarea_get_text(p->input), ""));
    settled_path(p);
    assert(!strcmp(ui_path_directory(p), child));
    assert(ui_path_list(p) == p->files && ui_path_location_input(p) == p->location);
    assert(ui_table_input(p->files) && path_find(p, "inside.txt") < BROWSER_LIMIT);
    /* Navigation uses the location field, without submitting a file. */
    before = accepted;
    lv_textarea_set_text(p->location, cwd);
    lv_group_focus_obj(p->location);
    assert(ui_overlay_key(40, 1, 0));
    assert(ui_path_loading(p) && accepted == before);
    settled_path(p);
    assert(!strcmp(ui_path_directory(p), cwd) && ui_table_focused(p->files));
    assert(path_find(p, ".hidden-dialog.txt") == BROWSER_LIMIT);
    /* Listing completion must not erase an application's validation message. */
    ui_table_select(p->files, path_find(p, "existing.txt"), 0, 1);
    ui_path_error(p, "Application validation stays visible.");
    lv_obj_add_state(p->hidden, LV_STATE_CHECKED);
    lv_obj_send_event(p->hidden, LV_EVENT_VALUE_CHANGED, NULL);
    assert(ui_path_loading(p));
    settled_path(p);
    assert(path_find(p, ".hidden-dialog.txt") < BROWSER_LIMIT);
    assert(ui_table_row(p->files) == path_find(p, "existing.txt"));
    assert(p->has_error && !strcmp(lv_label_get_text(p->status), "Application validation stays visible."));
    /* Failed navigation leaves the published list and the filename intact. */
    ui_path_set(p, "Київ.txt");
    row = p->listing.count;
    lv_obj_remove_state(p->hidden, LV_STATE_CHECKED);
    assert(path_navigate(p, "folder", 0, 1) && ui_path_loading(p));
    assert(!path_navigate(p, "missing-dialog-folder", 1, 1));
    assert(!ui_path_loading(p) && p->listing.count == row && !strcmp(ui_path_directory(p), cwd));
    assert(lv_obj_has_state(p->hidden, LV_STATE_CHECKED));
    assert(!lv_obj_has_state(ui_table_input(p->files), LV_STATE_DISABLED));
    assert(!lv_obj_has_state(p->accept, LV_STATE_DISABLED));
    assert(!strcmp(lv_textarea_get_text(p->input), "Київ.txt"));
    /* Home/End/Page navigation belongs to the shared list, not the app. */
    ui_table_focus(p->files);
    lv_group_send_data(gx_lv_group(), LV_KEY_END);
    assert(ui_table_row(p->files) == p->listing.count - 1);
    lv_group_send_data(gx_lv_group(), LV_KEY_HOME);
    assert(!ui_table_row(p->files));
    assert(ui_overlay_key(78, 1, 0) && ui_table_row(p->files) > 0);
    assert(ui_overlay_key(75, 1, 0) && !ui_table_row(p->files));
    /* Selection only fills the name; Enter activation is deferred until the
     * table event has returned, so closing the modal cannot free its sender. */
    row = path_find(p, "folder");
    assert(row < BROWSER_LIMIT);
    ui_table_select(p->files, row, 0, 1);
    assert(!strcmp(lv_textarea_get_text(p->input), "folder/"));
    lv_group_send_data(gx_lv_group(), LV_KEY_ENTER);
    assert(p->activation_timer && !ui_path_loading(p) && accepted == before);
    path_activate_tick(p->activation_timer);
    settled_path(p);
    assert(!strcmp(ui_path_directory(p), child));
    lv_group_send_data(gx_lv_group(), LV_KEY_BACKSPACE);
    settled_path(p);
    assert(!strcmp(ui_path_directory(p), cwd));
    row = path_find(p, "Київ.txt");
    ui_table_select(p->files, row, 0, 1);
    lv_group_send_data(gx_lv_group(), LV_KEY_ENTER);
    assert(p->activation_timer && accepted == before);
    path_activate_tick(p->activation_timer);
    assert(!ui_overlay_active() && accepted == before + 1);
    assert(!browser_join(cwd, "Київ.txt", full) && !strcmp(received, full));
    /* Saving retains an owned basename when the source is a substring of
     * LVGL's textarea buffer. Up/Home previously exposed a freed substring. */
    assert(!browser_join(child, "draft.txt", full));
    p = ui_path_open("Save", UI_PATH_SAVE, full, path_result, &accepted);
    settled_path(p);
    assert(!strcmp(ui_path_directory(p), child));
    assert(!strcmp(lv_textarea_get_text(p->input), "draft.txt"));
    ui_path_set(p, full);
    lv_obj_send_event(p->up, LV_EVENT_CLICKED, NULL);
    settled_path(p);
    assert(!strcmp(ui_path_directory(p), cwd));
    assert(!strcmp(lv_textarea_get_text(p->input), "draft.txt"));
    if (getenv("HOME") && !strcmp(getenv("HOME"), cwd)) {
        ui_path_set(p, full);
        lv_obj_send_event(p->home, LV_EVENT_CLICKED, NULL);
        settled_path(p);
        assert(!strcmp(ui_path_directory(p), cwd));
        assert(!strcmp(lv_textarea_get_text(p->input), "draft.txt"));
    }
    /* Dialogs adapt to small calculator windows as well as normal apps at
     * either physical density; the list remains a usable viewport. */
    for (scale = 1; scale <= 2; scale++) {
        gx_lv_apply_density(scale);
        for (size = 0; size < 2; size++) {
            unsigned width = size ? 880 : 540, height = size ? 600 : 360;
            lv_display_set_resolution(lv_display_get_default(), width * scale, height * scale);
            ui_path_density(p);
            ui_data_resize();
            lv_obj_update_layout(lv_layer_top());
            frame = lv_obj_get_parent(p->body);
            lv_obj_get_coords(frame, &bounds);
            assert(bounds.x1 >= 0 && bounds.y1 >= 0);
            assert(bounds.x2 < (int32_t)(width * scale) && bounds.y2 < (int32_t)(height * scale));
            assert(lv_obj_get_content_height(ui_table_input(p->files)) >= gx_lv_px(UI_ROW_HEIGHT));
            assert(lv_obj_get_width(p->location) > gx_lv_px(80));
            assert(!strcmp(lv_textarea_get_text(p->input), "draft.txt"));
        }
    }
    ui_path_cancel(p);
    lv_display_set_resolution(lv_display_get_default(), old_width, old_height);
    /* Escape can tear down enumeration or deferred activation safely. */
    p = ui_path_open("Cancel reading", UI_PATH_OPEN, cwd, path_result, &accepted);
    assert(ui_path_loading(p));
    before = cancelled;
    ui_overlay_key(41, 1, 0);
    assert(!ui_overlay_active() && cancelled == before + 1);
    assert(!browser_join(cwd, "existing.txt", full));
    p = ui_path_open("Cancel activation", UI_PATH_OPEN, full, path_result, &accepted);
    settled_path(p);
    assert(ui_table_row(p->files) == path_find(p, "existing.txt"));
    path_activate(p, path_find(p, "existing.txt"));
    assert(p->activation_timer);
    ui_overlay_close();
    assert(!ui_overlay_active() && cancelled == before + 2);
}
static void progress_checks(void) {
    ui_progress *p = ui_progress_create(lv_screen_active(), cancel_job, &cancelled_jobs);
    unsigned scale;
    uint32_t done;
    assert(p);
    gx_lv_width(p->root, 320);
    ui_progress_update(p, "Loading", 0, 0);
    assert(lv_obj_is_hidden(p->track));
    for (scale = 1; scale <= 2; scale++) {
        gx_lv_apply_density(scale);
        ui_progress_density(p);
        gx_lv_width(p->root, 320);
        for (done = 0; done <= 10; done++) {
            ui_progress_update(p, "Working", done, 10);
            lv_obj_update_layout(p->root);
            assert(p->permille >= done * 100 - (done ? 1 : 0));
            assert(p->permille <= done * 100 + 1);
            assert(lv_obj_get_width(p->fill) <= lv_obj_get_content_width(p->track));
            assert(lv_obj_get_height(p->track) == gx_lv_px(4));
        }
        ui_progress_update(p, "Large counts", 2147483647UL, 4294967295UL);
        assert(p->permille >= 499 && p->permille <= 500);
        ui_progress_update(p, "Complete", 42, 3);
        assert(p->permille == 1000);
    }
    lv_obj_send_event(p->cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_send_event(p->cancel, LV_EVENT_CLICKED, NULL);
    assert(cancelled_jobs == 1 && p->cancelled && lv_obj_has_state(p->cancel, LV_STATE_DISABLED));
    ui_progress_running(p, 1);
    assert(!p->cancelled && !lv_obj_has_state(p->cancel, LV_STATE_DISABLED));
    assert(ui_progress_focus_cancel(p) && lv_group_get_focused(gx_lv_group()) == p->cancel);
    ui_progress_running(p, 0);
    assert(!ui_progress_focus_cancel(p));
    lv_obj_send_event(p->cancel, LV_EVENT_CLICKED, NULL);
    assert(cancelled_jobs == 1 && !p->cancelled);
    lv_obj_delete(p->root);
    p = ui_progress_create(lv_screen_active(), NULL, NULL);
    assert(p && !p->cancel);
    assert(!ui_progress_focus_cancel(p));
    lv_obj_delete(p->root);
}
static void progress_focus_checks(void) {
    lv_obj_t *panel = ui_panel(lv_screen_active());
    lv_obj_t *previous = ui_button(panel, "Previous control", UI_BUTTON);
    lv_obj_t *fallback = ui_button(lv_screen_active(), "Fallback control", UI_BUTTON);
    lv_obj_t *replacement;
    ui_progress *p = ui_progress_create(lv_screen_active(), cancel_job, &cancelled_jobs);
    assert(p);
    lv_group_focus_obj(previous);
    ui_progress_running(p, 1);
    assert(ui_progress_focus_cancel(p) && p->saved == previous);
    assert(ui_progress_focus_cancel(p) && p->saved == previous);
    ui_progress_running(p, 0);
    lv_obj_set_hidden(p->root, true);
    assert(lv_group_get_focused(gx_lv_group()) == previous && !p->saved);
    lv_obj_set_hidden(p->root, false);
    ui_progress_running(p, 1);
    assert(ui_progress_focus_cancel(p));
    lv_obj_set_hidden(panel, true);
    ui_progress_running(p, 0);
    assert(lv_group_get_focused(gx_lv_group()) == fallback);
    lv_obj_set_hidden(panel, false);
    lv_group_focus_obj(previous);
    ui_progress_running(p, 1);
    assert(ui_progress_focus_cancel(p) && p->saved == previous);
    lv_obj_delete(previous);
    assert(!p->saved);
    replacement = ui_button(panel, "Replacement control", UI_BUTTON);
    ui_progress_running(p, 0);
    assert(lv_group_get_focused(gx_lv_group()) != p->cancel);
    assert(progress_focusable(lv_group_get_focused(gx_lv_group())));
    lv_group_focus_obj(fallback);
    ui_progress_running(p, 1);
    assert(ui_progress_focus_cancel(p));
    lv_group_focus_obj(replacement);
    ui_progress_running(p, 0);
    assert(lv_group_get_focused(gx_lv_group()) == replacement && !p->saved);
    ui_progress_running(p, 1);
    assert(ui_progress_focus_cancel(p));
    lv_obj_delete(p->root);
    assert(lv_group_get_focused(gx_lv_group()) == replacement);
    lv_obj_delete(panel);
    lv_obj_delete(fallback);
}
int main(void) {
    assert(!gx_lv_open("Forms component checks", 800, 560));
    ui_screen();
    choice_checks();
    path_checks();
    file_dialog_checks();
    progress_checks();
    progress_focus_checks();
    gx_lv_close();
    puts("FORMS_COMPONENT passed: choice, file browsing/navigation/lifecycle/geometry, path validation, progress");
    return 0;
}
