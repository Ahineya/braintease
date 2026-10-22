/* SPDX-License-Identifier: MIT */
/* Independent LVGL implementation of nxselect's one-line selection protocol. */
#include "ui.h"
#include "browser.h"
#include <gx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#define ROW_HEIGHT UI_ROW_HEIGHT
static struct browser browser;
static lv_obj_t *path_field, *go_button, *up_button, *home_button, *refresh_button, *hidden_box;
static lv_obj_t *list, *detail, *status, *hint, *open_button, *cancel_button, *title;
static lv_obj_t **rows;
static int selected = -1, trace, accepted, replace_path, pending, fatal;
static unsigned width = 640, height = 480;
static lv_obj_t *header, *path_bar, *toolbar, *detail_bar, *footer;
static char result_path[BROWSER_PATH], pending_path[BROWSER_PATH];
static char pending_name[BROWSER_NAME];

static void message(const char *text) { lv_label_set_text(status, text); }
static void failure(const char *what) {
    char text[180];
    snprintf(text, sizeof(text), "%s: %s", what, strerror(errno));
    message(text);
    if (trace)
        fprintf(stderr, "SELECT_ERROR %s\n", text);
}
static void focus_path(void) {
    const unsigned char *p = (const unsigned char *)lv_textarea_get_text(path_field);
    uint32_t length = 0;
    while (*p) {
        if ((*p & 0xc0) != 0x80)
            length++;
        p++;
    }
    lv_group_focus_obj(path_field);
    replace_path = 1;
    lv_label_set_text_selection_start(lv_textarea_get_label(path_field), 0);
    lv_label_set_text_selection_end(lv_textarea_get_label(path_field), length);
}
static void count_status(void) {
    char text[100];
    snprintf(text, sizeof(text), "%u items%s", browser.count,
             browser.limited ? " (first 1024 shown)" : "");
    message(text);
}
static void select_row(int index, int scroll) {
    char text[BROWSER_NAME + 64];
    struct browser_entry *e;
    if (selected >= 0 && selected < (int)browser.count)
        lv_obj_remove_state(rows[selected], LV_STATE_CHECKED);
    if (index < 0 || index >= (int)browser.count) {
        selected = -1;
        lv_label_set_text(detail, "No file selected");
        return;
    }
    selected = index;
    e = &browser.entries[index];
    lv_obj_add_state(rows[index], LV_STATE_CHECKED);
    snprintf(text, sizeof(text), "%s%s", e->name, e->kind == BROWSER_DIRECTORY ? " /" : "");
    lv_label_set_text(detail, text);
    if (scroll)
        lv_obj_scroll_to_view(rows[index], LV_ANIM_OFF);
    if (trace)
        fprintf(stderr, "SELECT_ROW %d %s\n", index, e->name);
}
static void activate(void);
static void navigate(const char *path, const char *name);
static void move_selection(int amount) {
    int index = selected + amount;
    if (!browser.count)
        return;
    if (index < 0)
        index = 0;
    if (index >= (int)browser.count)
        index = browser.count - 1;
    select_row(index, 1);
}
static void list_key(lv_event_t *e) {
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_UP)
        move_selection(-1);
    else if (key == LV_KEY_DOWN)
        move_selection(1);
    else if (key == LV_KEY_HOME)
        select_row(0, 1);
    else if (key == LV_KEY_END)
        select_row(browser.count - 1, 1);
    else if (key == LV_KEY_ENTER)
        activate();
    else if (key == LV_KEY_BACKSPACE || key == LV_KEY_LEFT) {
        char parent[BROWSER_PATH];
        browser_parent(&browser, parent);
        navigate(parent, NULL);
    } else if (key == LV_KEY_RIGHT && selected >= 0 &&
               browser.entries[selected].kind == BROWSER_DIRECTORY)
        activate();
}
static void row_event(lv_event_t *e) {
    unsigned i = (unsigned)(uintptr_t)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        select_row(i, 0);
        lv_group_focus_obj(list);
    } else if (code == LV_EVENT_DOUBLE_CLICKED) {
        select_row(i, 0);
        activate();
    }
}
static void scroll_event(lv_event_t *e) {
    (void)e;
    if (trace)
        fprintf(stderr, "SELECT_SCROLL %ld\n", (long)lv_obj_get_scroll_y(list));
}
static void resize(unsigned w, unsigned h) {
    width = w;
    height = h;
    gx_lv_height(header, 24);
    gx_lv_height(path_bar, UI_CONTROL);
    ui_input_height(path_field, UI_CONTROL);
    gx_lv_width(go_button, 64);
    gx_lv_height(toolbar, UI_CONTROL);
    gx_lv_width(up_button, 64);
    gx_lv_width(home_button, 88);
    gx_lv_width(refresh_button, 104);
    gx_lv_height(detail_bar, 20);
    gx_lv_height(footer, UI_CONTROL);
    gx_lv_width(open_button, 88);
    gx_lv_width(cancel_button, 96);
    if (selected >= 0)
        lv_obj_scroll_to_view(rows[selected], LV_ANIM_OFF);
}
static void rebuild(const char *name) {
    unsigned i;
    int restore = 0;
    lv_obj_clean(list);
    free(rows);
    rows = calloc(browser.count ? browser.count : 1, sizeof(*rows));
    if (!rows) {
        message("Not enough memory");
        fatal = 1;
        gx_lv_finish();
        return;
    }
    selected = -1;
    for (i = 0; i < browser.count; i++) {
        char display[BROWSER_NAME], type[24];
        unsigned j;
        rows[i] = ui_button(list, NULL, UI_ROW);
        lv_group_remove_obj(rows[i]);
        lv_obj_set_width(rows[i], lv_pct(100));
        strcpy(display, browser.entries[i].name);
        for (j = 0; display[j]; j++)
            if ((unsigned char)display[j] < 32 || display[j] == 127)
                display[j] = '?';
        if (browser.entries[i].kind == BROWSER_DIRECTORY)
            strcpy(type, "Folder");
        else if (browser.entries[i].kind == BROWSER_FILE) {
            unsigned long n = browser.entries[i].size;
            if (n >= 1048576UL)
                snprintf(type, sizeof(type), "%lu MiB", n / 1048576UL);
            else if (n >= 1024UL)
                snprintf(type, sizeof(type), "%lu KiB", n / 1024UL);
            else
                snprintf(type, sizeof(type), "%lu B", n);
        } else
            strcpy(type, "Other");
        ui_row_icon_labels(rows[i],
                           browser.entries[i].kind == BROWSER_DIRECTORY ? "folder" : "file",
                           display, type);
        lv_obj_add_event_cb(rows[i], row_event, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_add_event_cb(rows[i], row_event, LV_EVENT_DOUBLE_CLICKED, (void *)(uintptr_t)i);
        if (name && !strcmp(name, browser.entries[i].name))
            restore = i;
    }
    lv_textarea_set_text(path_field, browser.path);
    replace_path = 0;
    lv_textarea_set_cursor_pos(path_field, LV_TEXTAREA_CURSOR_LAST);
    selected = browser.count ? restore : -1; /* layout before scrolling to the selection */
    resize(width, height);
    selected = -1;
    select_row(browser.count ? restore : -1, 1);
    count_status();
    lv_group_focus_obj(list);
    if (trace)
        fprintf(stderr, "SELECT_DIR %s count=%u hidden=%d limited=%d\n", browser.path,
                browser.count, browser.hidden, browser.limited);
}
static void navigation_task(void *unused) {
    (void)unused;
    pending = 0;
    if (browser_load(&browser, pending_path, lv_obj_has_state(hidden_box, LV_STATE_CHECKED)) < 0) {
        failure("Cannot open folder");
        if (browser.hidden)
            lv_obj_add_state(hidden_box, LV_STATE_CHECKED);
        else
            lv_obj_remove_state(hidden_box, LV_STATE_CHECKED);
        return;
    }
    rebuild(pending_name[0] ? pending_name : NULL);
}
static void navigate(const char *path, const char *name) {
    if (strlen(path) >= sizeof(pending_path)) {
        errno = ENAMETOOLONG;
        failure("Path");
        return;
    }
    strcpy(pending_path, path);
    pending_name[0] = 0;
    if (name) {
        strncpy(pending_name, name, sizeof(pending_name) - 1);
        pending_name[sizeof(pending_name) - 1] = 0;
    }
    if (!pending) {
        pending = 1;
        if (lv_async_call(navigation_task, NULL) != LV_RESULT_OK) {
            pending = 0;
            message("Not enough memory to open folder");
        }
    }
}
static void choose(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0) {
        failure("Cannot select file");
        return;
    }
    if (!S_ISREG(st.st_mode)) {
        message("Choose a regular file");
        return;
    }
    if (strlen(path) >= sizeof(result_path)) {
        errno = ENAMETOOLONG;
        failure("Path");
        return;
    }
    strcpy(result_path, path);
    accepted = 1;
    gx_lv_finish();
}
static void activate(void) {
    char full[BROWSER_PATH];
    if (selected < 0) {
        message("Select a file or enter a path");
        return;
    }
    if (browser_join(browser.path, browser.entries[selected].name, full) < 0) {
        failure("Path");
        return;
    }
    if (browser.entries[selected].kind == BROWSER_DIRECTORY)
        navigate(full, NULL);
    else
        choose(full);
}
static void open_path(void) {
    char full[BROWSER_PATH];
    struct stat st;
    if (!*lv_textarea_get_text(path_field)) {
        message("Enter a path");
        return;
    }
    if (browser_join(browser.path, lv_textarea_get_text(path_field), full) < 0) {
        failure("Path");
        return;
    }
    if (stat(full, &st) < 0) {
        failure("Cannot open path");
        return;
    }
    if (S_ISDIR(st.st_mode))
        navigate(full, NULL);
    else
        choose(full);
}
static void command(lv_event_t *e) {
    unsigned cmd = (unsigned)(uintptr_t)lv_event_get_user_data(e);
    char parent[BROWSER_PATH];
    const char *home;
    if (cmd == 0)
        open_path();
    else if (cmd == 1) {
        browser_parent(&browser, parent);
        navigate(parent, strrchr(browser.path, '/') + 1);
    } else if (cmd == 2) {
        home = getenv("HOME");
        if (home && *home)
            navigate(home, NULL);
        else
            message("Home folder is not set");
    } else if (cmd == 3)
        navigate(browser.path, selected >= 0 ? browser.entries[selected].name : NULL);
    else if (cmd == 4)
        activate();
    else if (cmd == 5)
        gx_lv_finish();
    else if (cmd == 6)
        navigate(browser.path, NULL);
}
static void path_key(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_PRESSED || lv_event_get_code(e) == LV_EVENT_DEFOCUSED) {
        replace_path = 0;
        lv_textarea_clear_selection(path_field);
        return;
    }
    uint32_t k = lv_event_get_key(e);
    if (k == LV_KEY_ENTER)
        open_path();
    else if (k == LV_KEY_DOWN)
        lv_group_focus_obj(list);
    else if (k == LV_KEY_LEFT || k == LV_KEY_RIGHT || k == LV_KEY_HOME || k == LV_KEY_END) {
        replace_path = 0;
        lv_textarea_clear_selection(path_field);
    } else if (k == LV_KEY_BACKSPACE || k == LV_KEY_DEL) {
        if (replace_path)
            lv_textarea_set_text(path_field, "");
        replace_path = 0;
    }
}
static void insert(const char *text) {
    unsigned long i, n = strlen(text),
                     old = replace_path ? 0 : strlen(lv_textarea_get_text(path_field));
    for (i = 0; i < n; i++)
        if ((unsigned char)text[i] < 32 || text[i] == 127) {
            message("Paste a single path");
            return;
        }
    if (n + old >= BROWSER_PATH) {
        message("Path is too long");
        return;
    }
    if (replace_path)
        lv_textarea_set_text(path_field, "");
    replace_path = 0;
    /* Keep LVGL's direct UTF-8 string path, not packed-scalar add_char. The
     * path limit above counts logical UTF-8 octets as the guest filesystem does. */
    lv_textarea_add_text(path_field, text);
    if (trace)
        fprintf(stderr, "SELECT_PATH %s\n", lv_textarea_get_text(path_field));
}
static void text(uint32_t scalar) {
    char utf8[5];
    if (lv_group_get_focused(gx_lv_group()) != path_field)
        return;
    if (scalar < 32 || scalar == 127)
        return;
    if (browser_utf8(scalar, utf8))
        insert(utf8);
}
static void action(unsigned code) {
    char buffer[BROWSER_PATH];
    if (lv_group_get_focused(gx_lv_group()) != path_field)
        return;
    if (code == GX_ACTION_SELECT_ALL)
        focus_path();
    else if (code == GX_ACTION_PASTE) {
        if (!gx_lv_clipboard(buffer, sizeof(buffer), 0))
            insert(buffer);
        else
            message("Clipboard path is too long or unavailable");
    } else if (code == GX_ACTION_COPY || code == GX_ACTION_CUT) {
        strcpy(buffer, lv_textarea_get_text(path_field));
        if (!gx_lv_clipboard(buffer, sizeof(buffer), 1) && code == GX_ACTION_CUT) {
            lv_textarea_set_text(path_field, "");
            replace_path = 0;
        }
    }
}
static int shortcut(unsigned code, int down, unsigned modifiers) {
    if (code == 41) {
        if (down)
            gx_lv_finish();
        return 1;
    }
    if (code == 62) {
        if (down)
            navigate(browser.path, selected >= 0 ? browser.entries[selected].name : NULL);
        return 1;
    }
    if (code == 15 && (modifiers & 1)) {
        if (down)
            focus_path();
        return 1;
    }
    if (code == 75 || code == 78) {
        if (down && lv_group_get_focused(gx_lv_group()) == list) {
            lv_obj_update_layout(list);
            int page = lv_obj_get_content_height(list) / gx_lv_px(ROW_HEIGHT);
            if (page < 1)
                page = 1;
            move_selection(code == 75 ? -page : page);
        }
        return 1;
    }
    return 0;
}
static lv_obj_t *button(lv_obj_t *parent, const char *name, unsigned id, unsigned role) {
    static const char *icons[] = {"arrow-right", "arrow-up", "house", "refresh-cw", "folder", "x"};
    lv_obj_t *b = ui_icon_button(parent, name, icons[id], role);
    lv_obj_add_event_cb(b, command, LV_EVENT_CLICKED, (void *)(uintptr_t)id);
    return b;
}
static void ui(void) {
    lv_obj_t *screen = lv_screen_active(), *spacer;
    ui_screen();
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    header = ui_stack(screen, LV_FLEX_FLOW_ROW, UI_SPACE);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    title = ui_label(header, "Open file", UI_HEADING);
    path_bar = ui_stack(screen, LV_FLEX_FLOW_ROW, UI_SPACE);
    lv_obj_set_flex_align(path_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    path_field = ui_input(path_bar, "Folder or file path");
    lv_obj_set_width(path_field, 0);
    lv_obj_set_flex_grow(path_field, 1);
    lv_obj_add_event_cb(path_field, path_key, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(path_field, path_key, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(path_field, path_key, LV_EVENT_DEFOCUSED, NULL);
    go_button = button(path_bar, "Go", 0, UI_BUTTON);
    toolbar = ui_stack(screen, LV_FLEX_FLOW_ROW, UI_SPACE);
    lv_obj_set_flex_align(toolbar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    up_button = button(toolbar, "Up", 1, UI_TOOL);
    home_button = button(toolbar, "Home", 2, UI_TOOL);
    refresh_button = button(toolbar, "Refresh", 3, UI_TOOL);
    spacer = ui_stack(toolbar, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_width(spacer, 0);
    lv_obj_set_height(spacer, 0);
    lv_obj_set_flex_grow(spacer, 1);
    hidden_box = ui_checkbox(toolbar, "Hidden files");
    lv_obj_add_event_cb(hidden_box, command, LV_EVENT_VALUE_CHANGED, (void *)(uintptr_t)6);
    list = ui_list(screen);
    lv_obj_set_width(list, lv_pct(100));
    lv_obj_set_height(list, 0);
    lv_obj_set_flex_grow(list, 1);
    lv_group_add_obj(gx_lv_group(), list);
    lv_obj_add_event_cb(list, list_key, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(list, scroll_event, LV_EVENT_SCROLL, NULL);
    detail_bar = ui_stack(screen, LV_FLEX_FLOW_ROW, UI_SPACE);
    lv_obj_set_flex_align(detail_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    detail = ui_label(detail_bar, "", UI_BODY);
    lv_obj_set_width(detail, 0);
    lv_obj_set_flex_grow(detail, 1);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_MODE_DOTS);
    status = ui_label(detail_bar, "", UI_SECONDARY);
    lv_obj_set_style_max_width(status, lv_pct(65), 0);
    lv_label_set_long_mode(status, LV_LABEL_LONG_MODE_DOTS);
    footer = ui_stack(screen, LV_FLEX_FLOW_ROW, UI_SPACE);
    lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    hint = ui_label(footer, "Ctrl+L Path  ·  F5 Refresh", UI_SECONDARY);
    lv_obj_set_width(hint, 0);
    lv_obj_set_flex_grow(hint, 1);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_DOTS);
    cancel_button = button(footer, "Cancel", 5, UI_BUTTON);
    open_button = button(footer, "Open", 4, UI_PRIMARY);
    if (browser.hidden)
        lv_obj_add_state(hidden_box, LV_STATE_CHECKED);
    rebuild(NULL);
}

int main(int argc, char **argv) {
    const char *initial = ".";
    int i, code = 0, has_path = 0;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--trace"))
            trace = 1;
        else if (!strcmp(argv[i], "--help")) {
            puts("Usage: lvselect [--trace] [DIRECTORY]\nChoose a file; prints its full path, or "
                 "[] on cancellation.\nArrows/Home/End/PageUp/PageDown: select; Enter: open; "
                 "Backspace: parent.\nTab/Shift-Tab: controls; Ctrl+L: path; F5: refresh; Esc: "
                 "cancel.");
            return 0;
        } else if (argv[i][0] == '-' || has_path) {
            fputs("lvselect: invalid arguments\n", stderr);
            return 1;
        } else {
            initial = argv[i];
            has_path = 1;
        }
    }
    if (browser_load(&browser, initial, 0) < 0) {
        perror("lvselect");
        return 1;
    }
    /* Standalone utility. Embedded file dialogs remain in their app's window. */
    gx_lv_application("org.riplix.FilePicker", "File Picker", "folder");
    gx_lv_minimum(480, 340);
    if (gx_lv_open("Select File", width, height) < 0) {
        browser_free(&browser);
        return 1;
    }
    ui();
    gx_lv_handlers(text, action, resize);
    gx_lv_shortcuts(shortcut);
    code = gx_lv_run();
    gx_lv_close();
    free(rows);
    browser_free(&browser);
    if (code || fatal)
        return 1;
    puts(accepted ? result_path : "[]");
    return 0;
}
