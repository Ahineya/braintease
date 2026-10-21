/* SPDX-License-Identifier: MIT */
#include "ui-forms.h"
#include "browser.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

struct ui_choice {
    lv_obj_t *root, *label;
    char **labels;
    unsigned count, selected;
};
struct ui_path {
    ui_dialog *dialog;
    lv_obj_t *body, *input, *status, *accept, *browse;
    unsigned mode, confirming, finished, has_error;
    char *pending;
    ui_path_fn callback;
    void *user;
    ui_path_browse_fn browse_callback;
    void *browse_user;
    lv_obj_t *location, *listing_status, *hidden, *up, *home, *go;
    ui_table *files;
    struct browser listing;
    browser_job *scan;
    lv_timer_t *scan_timer, *activation_timer;
    unsigned binding, resizing, focus_after;
    uint32_t activation_row;
    char resolved[UI_PATH_LIMIT], requested[UI_PATH_LIMIT];
};
struct ui_progress {
    lv_obj_t *root, *label, *track, *fill, *cancel, *saved;
    unsigned permille, cancelled;
    ui_progress_cancel_fn callback;
    void *user;
};
static char *copy_text(const char *text) {
    char *result;
    if (!text)
        return NULL;
    result = malloc(strlen(text) + 1);
    if (result)
        strcpy(result, text);
    return result;
}
static void choice_free_labels(char **labels, unsigned count) {
    unsigned i;
    if (!labels)
        return;
    for (i = 0; i < count; i++)
        free(labels[i]);
    free(labels);
}
void ui_choice_select(ui_choice *c, unsigned selected) {
    if (!c || selected >= c->count || selected == c->selected)
        return;
    c->selected = selected;
    lv_label_set_text(c->label, c->labels[selected]);
    lv_obj_send_event(c->root, LV_EVENT_VALUE_CHANGED, NULL);
}
static void choice_chosen(unsigned action, void *user) {
    ui_choice_select(user, action);
}
static void choice_event(lv_event_t *event) {
    ui_choice *c = lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_DELETE) {
        ui_overlay_close_for(c);
        choice_free_labels(c->labels, c->count);
        free(c);
    } else if (code == LV_EVENT_CLICKED && !lv_obj_has_state(c->root, LV_STATE_DISABLED)) {
        struct ui_menu_item *items = calloc(c->count, sizeof(*items));
        unsigned i;
        if (!items)
            return;
        for (i = 0; i < c->count; i++) {
            items[i].text = c->labels[i];
            items[i].icon = i == c->selected ? "arrow-right" : "minus";
            items[i].action = i;
        }
        if (ui_menu_open(c->root, items, c->count, choice_chosen, c)) {
            /* The shared menu creates one row per item and focuses its first
             * row. Its public LVGL parent gives us the current-choice row. */
            lv_obj_t *first = lv_group_get_focused(gx_lv_group());
            if (first) {
                lv_obj_t *row = lv_obj_get_child(lv_obj_get_parent(first), c->selected);
                lv_group_focus_obj(row);
                lv_obj_scroll_to_view(row, LV_ANIM_OFF);
            }
        }
        free(items);
    } else if (code == LV_EVENT_KEY && !lv_obj_has_state(c->root, LV_STATE_DISABLED)) {
        uint32_t key = lv_event_get_key(event);
        unsigned next = c->selected;
        if ((key == LV_KEY_DOWN || key == LV_KEY_RIGHT) && next + 1 < c->count)
            next++;
        else if ((key == LV_KEY_UP || key == LV_KEY_LEFT) && next)
            next--;
        else if (key == LV_KEY_HOME)
            next = 0;
        else if (key == LV_KEY_END)
            next = c->count - 1;
        ui_choice_select(c, next);
    }
}
int ui_choice_bind(ui_choice *c, const char *const *labels, unsigned count, unsigned selected) {
    char **copied;
    unsigned i;
    if (!c || !labels || !count || count > UI_CHOICE_LIMIT || selected >= count)
        return 0;
    copied = calloc(count, sizeof(*copied));
    if (!copied)
        return 0;
    for (i = 0; i < count; i++) {
        if (!labels[i] || !(copied[i] = copy_text(labels[i]))) {
            choice_free_labels(copied, count);
            return 0;
        }
    }
    ui_overlay_close_for(c);
    choice_free_labels(c->labels, c->count);
    c->labels = copied;
    c->count = count;
    c->selected = selected;
    lv_label_set_text(c->label, copied[selected]);
    return 1;
}
ui_choice *ui_choice_create(lv_obj_t *parent, const char *const *labels,
                            unsigned count, unsigned selected) {
    ui_choice *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->root = ui_button(parent, NULL, UI_BUTTON);
    lv_obj_set_flex_flow(c->root, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c->root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    c->label = ui_label(c->root, "", UI_BODY);
    lv_obj_set_width(c->label, 0);
    lv_obj_set_flex_grow(c->label, 1);
    lv_label_set_long_mode(c->label, LV_LABEL_LONG_MODE_DOTS);
    ui_label(c->root, LV_SYMBOL_DOWN, UI_SECONDARY);
    if (!ui_choice_bind(c, labels, count, selected)) {
        lv_obj_delete(c->root);
        free(c);
        return NULL;
    }
    lv_obj_add_event_cb(c->root, choice_event, LV_EVENT_ALL, c);
    return c;
}
lv_obj_t *ui_choice_object(ui_choice *c) { return c ? c->root : NULL; }
unsigned ui_choice_selected(ui_choice *c) { return c ? c->selected : 0; }
void ui_choice_density(ui_choice *c) {
    if (c)
        gx_lv_height(c->root, UI_CONTROL);
}

static void path_button_text(ui_path *p, const char *text) {
    lv_obj_t *label = lv_obj_get_child(p->accept, 0);
    lv_label_set_text(label, text);
}
static void path_reset(ui_path *p) {
    p->confirming = 0;
    p->has_error = 0;
    free(p->pending);
    p->pending = NULL;
    path_button_text(p, p->mode == UI_PATH_SAVE ? "Save" : "Open");
    lv_label_set_text(p->status, "");
    ui_input_error(p->input, 0);
}
void ui_path_error(ui_path *p, const char *message) {
    if (!p)
        return;
    p->has_error = 1;
    ui_input_error(p->input, 1);
    ui_text_role(p->status, UI_ERROR);
    lv_label_set_text(p->status, message && *message ? message : "Unable to use this path.");
    lv_group_focus_obj(p->input);
}
/* Does not mutate CWD: relative paths remain relative to the application. */
static int path_validate(ui_path *p, const char *text, int *exists) {
    struct stat st;
    char *parent, *slash;
    unsigned long length = strlen(text);
    int status, error;
    *exists = 0;
    if (!length) {
        ui_path_error(p, "Enter a file path.");
        return 0;
    }
    if (length >= UI_PATH_LIMIT) {
        ui_path_error(p, "The file path is too long.");
        return 0;
    }
    if (strpbrk(text, "\r\n\t")) {
        ui_path_error(p, "The file path contains a line break or tab.");
        return 0;
    }
    status = stat(text, &st);
    error = errno;
    if (!status) {
        if (!S_ISREG(st.st_mode)) {
            ui_path_error(p, "Choose a regular file, rather than a directory or device.");
            return 0;
        }
        *exists = 1;
        return 1;
    }
    if (p->mode == UI_PATH_OPEN || error != ENOENT) {
        ui_path_error(p, error == ENOENT ? "The file does not exist."
                                       : "Unable to access this file.");
        return 0;
    }
    if (text[length - 1] == '/') {
        ui_path_error(p, "Enter a file name after the directory.");
        return 0;
    }
    parent = copy_text(text);
    if (!parent) {
        ui_path_error(p, "Not enough memory to check the file path.");
        return 0;
    }
    slash = strrchr(parent, '/');
    if (!slash)
        strcpy(parent, ".");
    else if (slash == parent)
        parent[1] = 0;
    else
        *slash = 0;
    status = stat(parent, &st);
    free(parent);
    if (status || !S_ISDIR(st.st_mode)) {
        ui_path_error(p, "The parent directory does not exist or cannot be opened.");
        return 0;
    }
    return 1;
}
static int path_navigate(ui_path *p, const char *directory, int focus, int keep_name);
static void path_scan_cancel(ui_path *p) {
    if (p->scan_timer) {
        lv_timer_delete(p->scan_timer);
        p->scan_timer = NULL;
    }
    browser_scan_delete(p->scan);
    p->scan = NULL;
    if (p->activation_timer) {
        lv_timer_delete(p->activation_timer);
        p->activation_timer = NULL;
    }
}
static void path_listing_error(ui_path *p, const char *message) {
    ui_text_role(p->listing_status, UI_ERROR);
    lv_label_set_text(p->listing_status, message);
    ui_input_error(p->location, 1);
}
static void path_listing_restore(ui_path *p, const char *message) {
    path_listing_error(p, message);
    lv_textarea_set_text(p->location, p->listing.path);
    strcpy(p->requested, p->listing.path);
    if (p->listing.hidden)
        lv_obj_add_state(p->hidden, LV_STATE_CHECKED);
    else
        lv_obj_remove_state(p->hidden, LV_STATE_CHECKED);
    ui_table_enabled(p->files, 1);
    lv_obj_remove_state(p->accept, LV_STATE_DISABLED);
}
static void path_scan_tick(lv_timer_t *timer) {
    ui_path *p = lv_timer_get_user_data(timer);
    int result = browser_scan_step(p->scan, 64);
    char text[96];
    char previous[BROWSER_NAME];
    unsigned selected = 0, i;
    if (!result)
        return;
    previous[0] = 0;
    if (!strcmp(p->listing.path, browser_scan_path(p->scan)) &&
        ui_table_row(p->files) < p->listing.count)
        strcpy(previous, p->listing.entries[ui_table_row(p->files)].name);
    lv_timer_delete(p->scan_timer);
    p->scan_timer = NULL;
    if (result < 0 || browser_scan_commit(p->scan, &p->listing) < 0) {
        path_listing_restore(p, "Unable to read this folder. The previous folder was kept.");
    } else {
        const char *name = lv_textarea_get_text(p->input), *slash = strrchr(name, '/');
        if (slash) {
            unsigned length = slash - name;
            if (name[0] == '/' &&
                ((slash == name && !strcmp(p->listing.path, "/")) ||
                 (strlen(p->listing.path) == length && !strncmp(name, p->listing.path, length))))
                name = slash + 1;
            else
                name = "";
        }
        /* Keep the selected filename through refreshes, and align an initial
         * file path with its row without rewriting a typed Save name. */
        for (i = 0; i < p->listing.count; i++)
            if (!strcmp(p->listing.entries[i].name, previous))
                selected = i;
        for (i = 0; i < p->listing.count; i++)
            if (*name && !strcmp(p->listing.entries[i].name, name))
                selected = i;
        p->binding++;
        ui_table_bind(p->files, p->listing.count, 3);
        if (p->listing.count)
            ui_table_select(p->files, selected, 0, 1);
        p->binding--;
        lv_textarea_set_text(p->location, p->listing.path);
        ui_input_error(p->location, 0);
        ui_text_role(p->listing_status, UI_SECONDARY);
        if (p->listing.limited)
            snprintf(text, sizeof(text), "Showing the first %u entries", BROWSER_LIMIT);
        else
            snprintf(text, sizeof(text), "%u items", p->listing.count - (strcmp(p->listing.path, "/") ? 1 : 0));
        lv_label_set_text(p->listing_status, text);
    }
    browser_scan_delete(p->scan);
    p->scan = NULL;
    ui_table_enabled(p->files, 1);
    lv_obj_remove_state(p->accept, LV_STATE_DISABLED);
    if (p->focus_after)
        ui_table_focus(p->files);
}
static int path_navigate(ui_path *p, const char *directory, int focus, int keep_name) {
    browser_job *job;
    char absolute[UI_PATH_LIMIT];
    if (!directory || !*directory)
        directory = p->listing.path;
    path_scan_cancel(p);
    if (browser_join(p->listing.path, directory, absolute) < 0) {
        path_listing_restore(p, "The folder path is too long.");
        return 0;
    }
    job = browser_scan_begin(absolute, lv_obj_has_state(p->hidden, LV_STATE_CHECKED));
    if (!job) {
        path_listing_restore(p, "Unable to open this folder.");
        return 0;
    }
    p->scan = job;
    p->scan_timer = lv_timer_create(path_scan_tick, 1, p);
    if (!p->scan_timer) {
        path_scan_cancel(p);
        path_listing_restore(p, "Not enough memory to open the folder.");
        return 0;
    }
    if (!keep_name) {
        if (p->mode == UI_PATH_OPEN)
            ui_path_set(p, "");
        else {
            const char *name = lv_textarea_get_text(p->input), *slash = strrchr(name, '/');
            if (slash)
                ui_path_set(p, slash + 1);
        }
    }
    strcpy(p->requested, browser_scan_path(job));
    p->focus_after = !!focus;
    lv_textarea_set_text(p->location, p->requested);
    ui_input_error(p->location, 0);
    ui_text_role(p->listing_status, UI_SECONDARY);
    lv_label_set_text(p->listing_status, "Reading folder...");
    ui_table_enabled(p->files, 0);
    lv_obj_add_state(p->accept, LV_STATE_DISABLED);
    return 1;
}
static const char *path_value(void *user, uint32_t row, unsigned column) {
    ui_path *p = user;
    static char size[40];
    struct browser_entry *entry;
    if (row >= p->listing.count)
        return "";
    entry = &p->listing.entries[row];
    if (!column)
        return entry->name;
    if (column == 1)
        return entry->kind == BROWSER_DIRECTORY ? "Folder" : entry->kind == BROWSER_FILE ? "File" : "Other";
    if (entry->kind != BROWSER_FILE)
        return "";
    snprintf(size, sizeof(size), "%lu", entry->size);
    return size;
}
static void path_selected(void *user, uint32_t row, unsigned column) {
    ui_path *p = user;
    struct browser_entry *entry;
    char name[BROWSER_NAME + 2];
    (void)column;
    if (p->binding || p->scan || row >= p->listing.count)
        return;
    entry = &p->listing.entries[row];
    if (entry->kind == BROWSER_DIRECTORY && p->mode == UI_PATH_SAVE)
        return;
    strcpy(name, entry->name);
    if (entry->kind == BROWSER_DIRECTORY)
        strcat(name, "/");
    ui_path_set(p, name);
}
static void path_activate_tick(lv_timer_t *timer) {
    ui_path *p = lv_timer_get_user_data(timer);
    struct browser_entry *entry;
    char absolute[UI_PATH_LIMIT];
    lv_timer_delete(p->activation_timer);
    p->activation_timer = NULL;
    if (p->scan || p->activation_row >= p->listing.count)
        return;
    entry = &p->listing.entries[p->activation_row];
    if (browser_join(p->listing.path, entry->name, absolute) < 0) {
        ui_path_error(p, "The file path is too long.");
        return;
    }
    if (entry->kind == BROWSER_DIRECTORY)
        path_navigate(p, absolute, 1, 0);
    else if (entry->kind == BROWSER_FILE) {
        ui_path_set(p, absolute);
        ui_path_submit(p);
    } else
        ui_path_error(p, "Choose a regular file.");
}
static void path_activate(ui_path *p, uint32_t row) {
    if (p->scan || row >= p->listing.count)
        return;
    p->activation_row = row;
    if (!p->activation_timer)
        p->activation_timer = lv_timer_create(path_activate_tick, 1, p);
}
static int path_key(void *user, uint32_t key) {
    ui_path *p = user;
    if (key == LV_KEY_ENTER) {
        path_activate(p, ui_table_row(p->files));
        return 1;
    }
    if (key == LV_KEY_HOME || key == LV_KEY_END) {
        if (p->listing.count)
            ui_table_select(p->files, key == LV_KEY_HOME ? 0 : p->listing.count - 1, 0, 1);
        return 1;
    }
    if (key == LV_KEY_PREV || key == LV_KEY_NEXT)
        return 0;
    if (key == LV_KEY_BACKSPACE || key == LV_KEY_LEFT) {
        char parent[UI_PATH_LIMIT];
        browser_parent(&p->listing, parent);
        path_navigate(p, parent, 1, 0);
        return 1;
    }
    return 0;
}
static void path_double(lv_event_t *event) {
    ui_path *p = lv_event_get_user_data(event);
    lv_indev_t *indev = lv_indev_active();
    uint32_t row = ui_table_row(p->files);
    if (indev) {
        lv_area_t area;
        lv_point_t point;
        int32_t y;
        lv_obj_t *body = ui_table_input(p->files);
        lv_indev_get_point(indev, &point);
        lv_obj_get_content_coords(body, &area);
        if (point.x < area.x1 || point.x > area.x2 || point.y < area.y1 || point.y > area.y2)
            return;
        y = point.y - area.y1 + lv_obj_get_scroll_y(body);
        if (y < 0)
            return;
        row = y / gx_lv_px(UI_ROW_HEIGHT);
    }
    path_activate(p, row);
}
static void path_toolbar(lv_event_t *event) {
    ui_path *p = lv_event_get_user_data(event);
    lv_obj_t *target = lv_event_get_target_obj(event);
    char parent[UI_PATH_LIMIT];
    if (target == p->up) {
        char *slash;
        strcpy(parent, p->scan ? p->requested : p->listing.path);
        slash = strrchr(parent, '/');
        if (slash == parent)
            parent[1] = 0;
        else if (slash)
            *slash = 0;
        path_navigate(p, parent, 1, 0);
    } else if (target == p->home) {
        const char *home = getenv("HOME");
        path_navigate(p, home && *home ? home : "/", 1, 0);
    } else if (target == p->go)
        path_navigate(p, lv_textarea_get_text(p->location), 1, 0);
    else if (target == p->hidden)
        path_navigate(p, p->scan ? p->requested : p->listing.path, 0, 1);
}
static int path_action(ui_dialog *dialog, unsigned action, void *user) {
    ui_path *p = user;
    const char *text;
    int exists, accepted;
    (void)dialog;
    if (!action) {
        p->finished = 1;
        if (p->callback)
            p->callback(p, NULL, p->user);
        return 1;
    }
    if (lv_group_get_focused(gx_lv_group()) == p->location) {
        path_navigate(p, lv_textarea_get_text(p->location), 1, 0);
        return 0;
    }
    if (p->scan)
        return 0;
    text = lv_textarea_get_text(p->input);
    if (!*text) {
        ui_path_error(p, "Choose a file or enter its name.");
        return 0;
    }
    if (browser_join(p->listing.path, text, p->resolved) < 0) {
        ui_path_error(p, "The file path is too long.");
        return 0;
    }
    {
        struct stat st;
        if (!stat(p->resolved, &st) && S_ISDIR(st.st_mode)) {
            path_navigate(p, p->resolved, 1, 0);
            return 0;
        }
    }
    text = p->resolved;
    if (!path_validate(p, text, &exists))
        return 0;
    if (p->mode == UI_PATH_SAVE && exists &&
        (!p->confirming || !p->pending || strcmp(p->pending, text))) {
        free(p->pending);
        p->pending = copy_text(text);
        if (!p->pending) {
            ui_path_error(p, "Not enough memory to confirm replacement.");
            return 0;
        }
        p->confirming = 1;
        p->has_error = 0;
        ui_input_error(p->input, 0);
        ui_text_role(p->status, UI_SECONDARY);
        lv_label_set_text(p->status, "This file already exists. Choose Replace to overwrite it.");
        path_button_text(p, "Replace");
        lv_group_focus_obj(p->accept);
        return 0;
    }
    p->has_error = 0;
    accepted = !p->callback || p->callback(p, text, p->user);
    if (accepted)
        p->finished = 1;
    else if (!p->has_error)
        ui_path_error(p, "Unable to use this file. Choose another path or try again.");
    return accepted;
}
static void path_input_event(lv_event_t *event) {
    ui_path *p = lv_event_get_user_data(event);
    if (lv_event_get_code(event) == LV_EVENT_VALUE_CHANGED)
        path_reset(p);
}
static int path_raw_key(ui_dialog *dialog, unsigned code, int down, unsigned modifiers, void *user) {
    ui_path *p = user;
    (void)dialog;
    (void)modifiers;
    if (ui_table_focused(p->files) && (code == 75 || code == 78)) {
        if (down && !p->scan)
            ui_table_page(p->files, code == 75 ? -1 : 1);
        return 1;
    }
    return 0;
}
static void path_size_event(lv_event_t *event) {
    ui_path_density(lv_event_get_user_data(event));
}
static void path_delete(lv_event_t *event) {
    ui_path *p = lv_event_get_user_data(event);
    lv_obj_remove_event_cb_with_user_data(p->body, path_size_event, p);
    path_scan_cancel(p);
    /* Adapter/window teardown also owns cancellation of an attached picker. */
    if (!p->finished && p->callback)
        p->callback(p, NULL, p->user);
    if (p->files)
        lv_obj_delete(ui_table_object(p->files));
    browser_free(&p->listing);
    free(p->pending);
    free(p);
}
ui_path *ui_path_open(const char *title, unsigned mode, const char *initial,
                       ui_path_fn accept, void *user) {
    ui_path *p;
    if (mode != UI_PATH_OPEN && mode != UI_PATH_SAVE)
        return NULL;
    p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->dialog = ui_dialog_open(title, 720, path_action, p);
    if (!p->dialog) {
        free(p);
        return NULL;
    }
    p->mode = mode;
    p->resizing = 1;
    p->callback = accept;
    p->user = user;
    p->body = ui_dialog_body(p->dialog);
    {
        lv_obj_t *toolbar = ui_stack(p->body, LV_FLEX_FLOW_ROW, UI_SPACE);
        lv_obj_t *row;
        lv_obj_set_flex_align(toolbar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        p->up = ui_icon_button(toolbar, "Up", "arrow-up", UI_BUTTON);
        p->home = ui_icon_button(toolbar, "Home", "house", UI_BUTTON);
        p->location = ui_input(toolbar, "Folder path");
        lv_obj_set_width(p->location, 0);
        lv_obj_set_flex_grow(p->location, 1);
        p->go = ui_button(toolbar, "Go", UI_BUTTON);
        row = ui_stack(p->body, LV_FLEX_FLOW_ROW, UI_SPACE);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        p->hidden = ui_checkbox(row, "Hidden files");
        p->listing_status = ui_label(row, "", UI_SECONDARY);
        lv_obj_set_width(p->listing_status, 0);
        lv_obj_set_flex_grow(p->listing_status, 1);
        lv_label_set_long_mode(p->listing_status, LV_LABEL_LONG_MODE_DOTS);
        p->files = ui_table_create(p->body, path_value, p);
        if (!p->files) {
            ui_overlay_close();
            free(p);
            return NULL;
        }
        lv_obj_set_height(ui_table_object(p->files), 0);
        lv_obj_set_flex_grow(ui_table_object(p->files), 1);
        ui_table_heading(p->files, 0, "Name");
        ui_table_heading(p->files, 1, "Kind");
        ui_table_heading(p->files, 2, "Size");
        ui_table_callbacks(p->files, path_selected, NULL);
        ui_table_keys(p->files, path_key);
        lv_obj_add_event_cb(ui_table_input(p->files), path_double, LV_EVENT_DOUBLE_CLICKED, p);
        row = ui_stack(p->body, LV_FLEX_FLOW_ROW, UI_SPACE);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        ui_label(row, mode == UI_PATH_SAVE ? "Save as" : "File", UI_SECONDARY);
        p->input = ui_input(row, "File name or full path");
        lv_obj_set_width(p->input, 0);
        lv_obj_set_flex_grow(p->input, 1);
        lv_obj_add_event_cb(p->up, path_toolbar, LV_EVENT_CLICKED, p);
        lv_obj_add_event_cb(p->home, path_toolbar, LV_EVENT_CLICKED, p);
        lv_obj_add_event_cb(p->go, path_toolbar, LV_EVENT_CLICKED, p);
        lv_obj_add_event_cb(p->hidden, path_toolbar, LV_EVENT_VALUE_CHANGED, p);
    }
    if (!getcwd(p->listing.path, sizeof(p->listing.path))) {
        const char *home = getenv("HOME");
        strcpy(p->listing.path, home && *home && strlen(home) < BROWSER_PATH ? home : "/");
    }
    strcpy(p->requested, p->listing.path);
    if (initial && *initial && strlen(initial) < UI_PATH_LIMIT) {
        struct stat st;
        char full[UI_PATH_LIMIT], *slash;
        if (!browser_join(p->listing.path, initial, full)) {
            if (!stat(full, &st) && S_ISDIR(st.st_mode))
                strcpy(p->requested, full);
            else {
                slash = strrchr(full, '/');
                if (slash) {
                    lv_textarea_set_text(p->input, slash + 1);
                    if (slash == full)
                        full[1] = 0;
                    else
                        *slash = 0;
                    if (!stat(full, &st) && S_ISDIR(st.st_mode))
                        strcpy(p->requested, full);
                    else
                        lv_textarea_set_text(p->input, initial);
                } else
                    lv_textarea_set_text(p->input, initial);
            }
        } else
            lv_textarea_set_text(p->input, initial);
    } else
        lv_textarea_set_text(p->input, initial ? initial : "");
    p->status = ui_label(p->body, "", UI_SECONDARY);
    lv_obj_set_width(p->status, lv_pct(100));
    lv_label_set_long_mode(p->status, LV_LABEL_LONG_MODE_WRAP);
    gx_lv_height(p->status, 20);
    ui_dialog_button(p->dialog, "Cancel", 0, 0);
    p->accept = ui_dialog_button(p->dialog, mode == UI_PATH_SAVE ? "Save" : "Open", 1, 1);
    lv_obj_add_event_cb(p->input, path_input_event, LV_EVENT_VALUE_CHANGED, p);
    lv_obj_add_event_cb(p->body, path_delete, LV_EVENT_DELETE, p);
    lv_obj_add_event_cb(p->body, path_size_event, LV_EVENT_SIZE_CHANGED, p);
    ui_dialog_keys(p->dialog, path_raw_key);
    p->resizing = 0;
    ui_path_density(p);
    path_navigate(p, p->requested, 0, 1);
    lv_group_focus_obj(p->input);
    return p;
}
lv_obj_t *ui_path_input(ui_path *p) { return p ? p->input : NULL; }
ui_table *ui_path_list(ui_path *p) { return p ? p->files : NULL; }
lv_obj_t *ui_path_location_input(ui_path *p) { return p ? p->location : NULL; }
const char *ui_path_directory(ui_path *p) { return p ? p->listing.path : ""; }
int ui_path_loading(ui_path *p) { return p && p->scan; }
void ui_path_set(ui_path *p, const char *text) {
    if (p) {
        char *copy = copy_text(text ? text : "");
        if (copy) {
            lv_textarea_set_text(p->input, copy);
            free(copy);
        }
    }
}
static void path_browse_event(lv_event_t *event) {
    ui_path *p = lv_event_get_user_data(event);
    if (!lv_obj_has_state(p->browse, LV_STATE_DISABLED) && p->browse_callback)
        p->browse_callback(p, p->browse_user);
}
lv_obj_t *ui_path_browse(ui_path *p, ui_path_browse_fn callback, void *user) {
    if (!p)
        return NULL;
    p->browse_callback = callback;
    p->browse_user = user;
    if (!p->browse) {
        p->browse = ui_icon_button(p->body, "Browse...", "folder", UI_BUTTON);
        lv_obj_add_event_cb(p->browse, path_browse_event, LV_EVENT_CLICKED, p);
    }
    return p->browse;
}
void ui_path_submit(ui_path *p) {
    if (p) {
        lv_group_focus_obj(p->input);
        ui_dialog_submit(p->dialog, 1);
    }
}
void ui_path_cancel(ui_path *p) {
    if (p)
        ui_dialog_submit(p->dialog, 0);
}
void ui_path_density(ui_path *p) {
    unsigned height, width;
    if (!p || p->resizing || !p->files)
        return;
    p->resizing = 1;
    height = lv_display_get_vertical_resolution(lv_display_get_default()) / gx_lv_density();
    height = height > 136 ? height - 136 : 144;
    if (height > 420)
        height = 420;
    if (height < 144)
        height = 144;
    gx_lv_height(p->body, height);
    gx_lv_height(p->status, 20);
    ui_input_height(p->input, UI_CONTROL);
    ui_input_height(p->location, UI_CONTROL);
    gx_lv_width(p->up, 64);
    gx_lv_width(p->home, 88);
    gx_lv_width(p->go, 64);
    lv_obj_update_layout(p->body);
    width = lv_obj_get_content_width(p->body) / gx_lv_density();
    p->binding++;
    ui_table_column_width(p->files, 0, width > 212 ? width - 212 : 64);
    ui_table_column_width(p->files, 1, 68);
    ui_table_column_width(p->files, 2, 88);
    ui_table_density(p->files);
    p->binding--;
    p->resizing = 0;
}

static void progress_geometry(ui_progress *p) {
    int32_t width = lv_obj_get_content_width(p->track);
    if (width < 0)
        width = 0;
    lv_obj_set_width(p->fill, (uint32_t)width * p->permille / 1000UL);
}
static void progress_track_event(lv_event_t *event) {
    progress_geometry(lv_event_get_user_data(event));
}
static void progress_previous_deleted(lv_event_t *event) {
    ui_progress *p = lv_event_get_user_data(event);
    p->saved = NULL;
}
static void progress_forget_focus(ui_progress *p) {
    if (p->saved) {
        lv_obj_remove_event_cb_with_user_data(p->saved, progress_previous_deleted, p);
        p->saved = NULL;
    }
}
static int progress_focusable(lv_obj_t *obj) {
    if (!obj)
        return 0;
    for (; obj; obj = lv_obj_get_parent(obj))
        if (lv_obj_is_hidden(obj) || lv_obj_has_state(obj, LV_STATE_DISABLED))
            return 0;
    return 1;
}
static void progress_restore_focus(ui_progress *p) {
    lv_group_t *group = p->cancel ? lv_obj_get_group(p->cancel) : NULL;
    if (group && lv_group_get_focused(group) == p->cancel) {
        if (p->saved && lv_obj_get_group(p->saved) == group && progress_focusable(p->saved))
            lv_group_focus_obj(p->saved);
        else {
            uint32_t count = lv_group_get_obj_count(group), i;
            /* Stock traversal skips a hidden control; also reject controls
             * whose ancestor is hidden/disabled before ending traversal. */
            for (i = 0; i < count; i++) {
                lv_group_focus_next(group);
                if (progress_focusable(lv_group_get_focused(group)))
                    break;
            }
            if (i == count)
                ui_focus_clear(group);
        }
    }
    progress_forget_focus(p);
}
static void progress_delete(lv_event_t *event) {
    ui_progress *p = lv_event_get_user_data(event);
    ui_progress_running(p, 0);
    free(p);
}
static void progress_cancel_event(lv_event_t *event) {
    ui_progress *p = lv_event_get_user_data(event);
    if (p->cancelled || lv_obj_has_state(p->cancel, LV_STATE_DISABLED))
        return;
    p->cancelled = 1;
    lv_obj_add_state(p->cancel, LV_STATE_DISABLED);
    if (p->callback)
        p->callback(p, p->user);
}
ui_progress *ui_progress_create(lv_obj_t *parent, ui_progress_cancel_fn cancel, void *user) {
    ui_progress *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->callback = cancel;
    p->user = user;
    p->root = ui_stack(parent, LV_FLEX_FLOW_COLUMN, UI_SPACE);
    p->label = ui_label(p->root, "", UI_SECONDARY);
    lv_obj_set_width(p->label, lv_pct(100));
    lv_label_set_long_mode(p->label, LV_LABEL_LONG_MODE_WRAP);
    p->track = ui_panel(p->root);
    lv_obj_set_width(p->track, lv_pct(100));
    lv_obj_set_style_bg_color(p->track, ui_colour(UI_BORDER), 0);
    lv_obj_set_style_border_width(p->track, 0, 0);
    lv_obj_set_style_radius(p->track, 0, 0);
    lv_obj_set_style_pad_all(p->track, 0, 0);
    lv_obj_set_scrollable(p->track, false);
    p->fill = ui_panel(p->track);
    lv_obj_set_style_bg_color(p->fill, ui_colour(UI_ACCENT), 0);
    lv_obj_set_style_border_width(p->fill, 0, 0);
    lv_obj_set_style_radius(p->fill, 0, 0);
    lv_obj_set_style_pad_all(p->fill, 0, 0);
    lv_obj_set_scrollable(p->fill, false);
    lv_obj_set_height(p->fill, lv_pct(100));
    lv_obj_set_width(p->fill, 0);
    if (cancel) {
        p->cancel = ui_button(p->root, "Cancel", UI_BUTTON);
        lv_obj_add_event_cb(p->cancel, progress_cancel_event, LV_EVENT_CLICKED, p);
    }
    lv_obj_add_event_cb(p->track, progress_track_event, LV_EVENT_SIZE_CHANGED, p);
    lv_obj_add_event_cb(p->root, progress_delete, LV_EVENT_DELETE, p);
    ui_progress_density(p);
    lv_obj_set_hidden(p->track, true);
    return p;
}
lv_obj_t *ui_progress_object(ui_progress *p) { return p ? p->root : NULL; }
void ui_progress_update(ui_progress *p, const char *text, uint32_t done, uint32_t total) {
    unsigned i, fraction = 0;
    uint32_t remainder = done;
    if (!p)
        return;
    lv_label_set_text(p->label, text ? text : "");
    if (!total) {
        lv_obj_set_hidden(p->track, true);
        p->permille = 0;
    } else {
        lv_obj_set_hidden(p->track, false);
        if (done >= total)
            p->permille = 1000;
        else {
            /* Ten-bit division avoids overflowing done*1000 on C16 long. */
            for (i = 0; i < 10; i++) {
                fraction <<= 1;
                if (remainder >= total - remainder) {
                    remainder -= total - remainder;
                    fraction++;
                } else
                    remainder += remainder;
            }
            p->permille = (fraction * 1000UL + 512UL) / 1024UL;
        }
    }
    progress_geometry(p);
}
int ui_progress_cancelled(ui_progress *p) { return p ? p->cancelled : 0; }
void ui_progress_running(ui_progress *p, int running) {
    if (!p)
        return;
    if (running)
        p->cancelled = 0;
    if (p->cancel) {
        if (running)
            lv_obj_remove_state(p->cancel, LV_STATE_DISABLED);
        else
            lv_obj_add_state(p->cancel, LV_STATE_DISABLED);
    }
    if (!running)
        progress_restore_focus(p);
}
int ui_progress_focus_cancel(ui_progress *p) {
    lv_group_t *group;
    lv_obj_t *current;
    if (!p || !p->cancel || lv_obj_has_state(p->cancel, LV_STATE_DISABLED))
        return 0;
    group = lv_obj_get_group(p->cancel);
    if (!group || !progress_focusable(p->cancel))
        return 0;
    current = lv_group_get_focused(group);
    if (current != p->cancel) {
        progress_forget_focus(p);
        p->saved = current;
        if (current)
            lv_obj_add_event_cb(current, progress_previous_deleted, LV_EVENT_DELETE, p);
    }
    lv_group_focus_obj(p->cancel);
    return lv_group_get_focused(group) == p->cancel;
}
void ui_progress_density(ui_progress *p) {
    if (p) {
        gx_lv_height(p->track, 4);
        progress_geometry(p);
    }
}
