/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FuseDE's guest shell. The compositor owns windows; this process owns shell UI. */
#include "ui.h"
#include "shell-catalog.h"
#include "icons/data.h"
#include <gx.h>
#include <gx-session.h>
#include <gx-panel.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__RIPPLE__) || defined(FUSE_X_USER)
typedef char panel_c16_layout[(sizeof(struct gx_panel) == 14) ? 1 : -1];
#endif
struct DockApp {
    const ShellApp *catalog;
    char id[80], name[80], icon[128];
    unsigned windows;
    int focused, pinned;
    lv_obj_t *button;
};
static ShellCatalog *catalog;
static struct gx_session_record *windows;
static size_t window_count;
static struct DockApp *apps;
static size_t app_count;
static uint32_t authority, revision;
static unsigned width, height;
static lv_obj_t *dock, *brand, *clock_panel, *clock_label, *menu;
static lv_obj_t *search, *menu_list, *menu_status, *category_list, *selected_row;
static lv_obj_t *run_title, *run_input, *run_terminal, *run_status, *run_button, *run_cancel_button;
static lv_obj_t *replace_input;
/* Up to 1024 Unicode characters, each encoded in at most four UTF-8 cells. */
static char run_command[4097];
static pid_t run_pid;
static int run_in_terminal;
static char pins_path[512], category[128], menu_app[80], notice[256];
static int menu_kind, trace, pending_refresh, right_pressed;
static long clock_minute = -1;
static unsigned menu_x, menu_y, menu_width, menu_height;
static void layout(void);
static void refresh_dock(void);
static void hide_menu(void);
static void application_menu(void);
static void run_dialog(void);
static void show_run(lv_event_t *event);
static void run_submit(void);
static void run_geometry(void);
static void window_menu(const char *id);

static void shell_error(const char *prefix, const char *detail) {
    snprintf(notice, sizeof(notice), "%s: %s", prefix, detail ? detail : "operation failed");
    if (trace) fprintf(stderr, "FUSEDE_ERROR %s\n", notice);
    if (menu_status) lv_label_set_text(menu_status, notice);
}
static void box(lv_obj_t *obj, int x, int y, int w, int h) {
    gx_lv_pos(obj, x, y);
    gx_lv_size(obj, w, h);
}
static void quiet(lv_obj_t *obj) {
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scrollable(obj, false);
}
static int inside(unsigned x, unsigned y, lv_obj_t *obj) {
    lv_area_t a;
    int32_t px = (int32_t)x * gx_lv_density(), py = (int32_t)y * gx_lv_density();
    int32_t radius, limit, dx = 0, dy = 0;
    lv_obj_get_coords(obj, &a);
    if (px < a.x1 || py < a.y1 || px > a.x2 || py > a.y2) return 0;
    if (!gx_lv_panel_alpha()) return 1;
    radius = lv_obj_get_style_radius(obj, LV_PART_MAIN);
    limit = (a.x2 - a.x1 + 1) / 2;
    if ((a.y2 - a.y1 + 1) / 2 < limit) limit = (a.y2 - a.y1 + 1) / 2;
    if (radius > limit) radius = limit;
    if (radius <= 0) return 1;
    /* Pixel centres follow the rounded geometry. A modal corner is outside
     * its panel, even though the compositor still routes dismissal to us. */
    if (px < a.x1 + radius) dx = 2 * (a.x1 + radius - px) - 1;
    else if (px > a.x2 - radius) dx = 2 * (px - (a.x2 + 1 - radius)) + 1;
    if (py < a.y1 + radius) dy = 2 * (a.y1 + radius - py) - 1;
    else if (py > a.y2 - radius) dy = 2 * (py - (a.y2 + 1 - radius)) + 1;
    return !dx || !dy || dx * dx + dy * dy <= 4 * radius * radius;
}
static void trace_control(const char *kind, const char *id, lv_obj_t *obj) {
    lv_area_t a;
    if (!trace || !obj) return;
    lv_obj_get_coords(obj, &a);
    printf("FUSEDE_CONTROL|%s|%s|%ld|%ld|%ld|%ld\n", kind, id,
           (long)(a.x1/gx_lv_density()), (long)(a.y1/gx_lv_density()),
           (long)((a.x2-a.x1+1)/gx_lv_density()), (long)((a.y2-a.y1+1)/gx_lv_density()));
}
static const char *first_label(lv_obj_t *obj) {
    unsigned i;
    if (lv_obj_check_type(obj, &lv_label_class)) return lv_label_get_text(obj);
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        const char *s = first_label(lv_obj_get_child(obj, i));
        if (s && *s) return s;
    }
    return "";
}
static void trace_menu(lv_obj_t *obj) {
    unsigned i;
    if (lv_obj_check_type(obj, &lv_button_class)) {
        const char *kind = "action", *id = first_label(obj);
        if (lv_obj_get_parent(obj) == menu_list) {
            const ShellApp *a = lv_obj_get_user_data(obj);
            if (a) { kind = "application"; id = a->id; }
        } else if (lv_obj_get_parent(obj) == category_list) kind = "category";
        trace_control(kind, id, obj);
        return;
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) trace_menu(lv_obj_get_child(obj, i));
}
static void regions(void) {
    struct gx_panel p;
    struct gx_panel_rect r[4];
    lv_area_t a;
    unsigned n = 0, i;
    lv_obj_t *objects[4];
    objects[n++] = brand;
    objects[n++] = clock_panel;
    objects[n++] = dock;
    if (menu) objects[n++] = menu;
    lv_obj_update_layout(lv_screen_active());
    memset(&p, 0, sizeof(p));
    for (i = 0; i < n; i++) {
        lv_obj_get_coords(objects[i], &a);
        r[i].x = a.x1 / gx_lv_density();
        r[i].y = a.y1 / gx_lv_density();
        r[i].width = (a.x2 - a.x1 + 1) / gx_lv_density();
        r[i].height = (a.y2 - a.y1 + 1) / gx_lv_density();
    }
    p.version = GXP_VERSION;
    p.operation = GXP_CONFIGURE;
    p.authority = authority;
    p.window = gx_lv_window();
    p.flags = menu ? GXP_KEYBOARD | GXP_MODAL : 0;
    if (gx_lv_panel_alpha()) p.flags |= GXP_ALPHA;
    p.count = n;
    p.regions = r;
    p.top = 40;
    p.bottom = 76;
    /* Output resize is asynchronous: the queued GX_RESIZE will replace these
     * regions after the surface acknowledges the new dimensions. */
    if (ioctl(gx_lv_fd(), GX_PANEL, &p) < 0 && errno != EAGAIN)
        shell_error("Panel", strerror(errno));
    trace_control("brand", "Riplix", brand);
    trace_control("clock", "clock", clock_panel);
    trace_control("applications", "Applications", lv_obj_get_child(dock, 0));
    if (menu) {
        trace_control("menu", "menu", menu);
        trace_control("search", "search", search);
        trace_control("run-title", "Run", run_title);
        trace_control("run-command", "Command", run_input);
        trace_control("run-terminal", "Run in terminal", run_terminal);
        if (run_status && !lv_obj_is_hidden(run_status))
            trace_control("run-status", "Status", run_status);
        trace_menu(menu);
    }
}
static int same_app(const struct gx_session_record *w, const char *id) {
    char key[80];
    if (w->app_id[0]) return !strcmp(w->app_id, id);
    snprintf(key, sizeof(key), "window-process-%lu", (unsigned long)w->process);
    return !strcmp(key, id);
}
static void command(uint32_t window, unsigned value) {
    struct gx_session q;
    memset(&q, 0, sizeof(q));
    q.version = GXS_VERSION;
    q.operation = GXS_CONTROL;
    q.authority = authority;
    q.object = window;
    q.value = value;
    if (ioctl(gx_lv_fd(), GX_SESSION, &q) < 0 && errno != ENOENT)
        shell_error("Window action", strerror(errno));
    pending_refresh = 1;
}
static void command_all(const char *id, unsigned value) {
    size_t i;
    for (i = 0; i < window_count; i++)
        if (same_app(&windows[i], id) && !(windows[i].flags & GXS_HIDDEN))
            command(windows[i].window, value);
}
static void launch(const ShellApp *app) {
    if (!app) return;
    if (shell_catalog_launch(catalog, app) < 0) {
        shell_error("Cannot launch", shell_catalog_error(catalog));
        if (!menu) application_menu();
        return;
    }
    hide_menu();
}
static void app_activate(lv_event_t *e) {
    unsigned n = (unsigned)(uintptr_t)lv_event_get_user_data(e);
    size_t i;
    if (n >= app_count) return;
    if (!apps[n].windows) launch(apps[n].catalog);
    else if (apps[n].windows > 1) window_menu(apps[n].id);
    else for (i = 0; i < window_count; i++)
        if (same_app(&windows[i], apps[n].id) && !(windows[i].flags & GXS_HIDDEN)) {
            command(windows[i].window, GXS_ACTIVATE);
            hide_menu();
            break;
        }
}
static void all_action(lv_event_t *e) {
    unsigned action = (unsigned)(uintptr_t)lv_event_get_user_data(e);
    char id[80];
    const ShellApp *app = shell_catalog_find(catalog, menu_app);
    strcpy(id, menu_app);
    if (action == 100) {
        int pinned = app ? shell_app_pinned(catalog, app) : 0;
        if (app && (shell_pin_set(catalog, app->id, !pinned) < 0 ||
                    shell_pins_save(catalog, pins_path) < 0)) {
            shell_error("Pins", shell_catalog_error(catalog));
            return;
        }
        hide_menu();
        refresh_dock();
        layout();
    } else if (action == 101) launch(app);
    else {
        command_all(id, action);
        hide_menu();
    }
}
static void choose_window(lv_event_t *e) {
    uint32_t id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    command(id, GXS_ACTIVATE);
    hide_menu();
}
static lv_obj_t *menu_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb,
                             unsigned long data) {
    lv_obj_t *b = ui_button(parent, text, UI_ROW);
    gx_lv_width(b, menu_width - 26);
    gx_lv_height(b, 32);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(uintptr_t)data);
    return b;
}
static void hide_menu(void) {
    if (!menu) return;
    gx_lv_cancel_key_repeat();
    if (run_input) {
        snprintf(run_command, sizeof(run_command), "%s", lv_textarea_get_text(run_input));
        run_in_terminal = lv_obj_has_state(run_terminal, LV_STATE_CHECKED);
    }
    /* Detach references before deleting grouped children. */
    search = menu_list = menu_status = category_list = selected_row = NULL;
    run_title = run_input = run_terminal = run_status = run_button = run_cancel_button = NULL;
    replace_input = NULL;
    lv_obj_delete(menu);
    menu = NULL;
    menu_kind = 0;
    regions();
}
static void create_menu(int kind, unsigned w, unsigned h, unsigned x, unsigned y) {
    hide_menu();
    menu_kind = kind;
    menu_width = w > width - 16 ? width - 16 : w;
    menu_height = h > height - 96 ? height - 96 : h;
    menu_x = x;
    if (menu_x + menu_width > width - 8) menu_x = width - 8 - menu_width;
    menu_y = y;
    if (menu_y + menu_height > height - 80) menu_y = height - 80 - menu_height;
    if (menu_y < 40) menu_y = 40;
    menu = ui_panel(lv_screen_active());
    lv_obj_set_style_radius(menu, gx_lv_px(10), 0);
    lv_obj_set_style_pad_all(menu, gx_lv_px(12), 0);
    lv_obj_set_style_pad_row(menu, gx_lv_px(4), 0);
    lv_obj_set_flex_flow(menu, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(menu, LV_DIR_VER);
    box(menu, menu_x, menu_y, menu_width, menu_height);
}
static void window_menu(const char *id) {
    size_t i, count = 0;
    const ShellApp *app = shell_catalog_find(catalog, id);
    char title[100];
    for (i = 0; i < window_count; i++)
        if (same_app(&windows[i], id) && !(windows[i].flags & GXS_HIDDEN)) count++;
    create_menu(2, 300, 50 + count * 36 + (app ? 72 : 0) + (count ? 108 : 0),
                width / 2 > 150 ? width / 2 - 150 : 8, height);
    strcpy(menu_app, id);
    ui_label(menu, app ? app->name : "Application windows", UI_HEADING);
    for (i = 0; i < window_count; i++) if (same_app(&windows[i], id) &&
                                         !(windows[i].flags & GXS_HIDDEN)) {
        snprintf(title, sizeof(title), "%s%s", windows[i].flags & GXS_MINIMIZED ? "[min] " : "",
                 windows[i].title[0] ? windows[i].title : "Untitled window");
        menu_button(menu, title, choose_window, windows[i].window);
    }
    if (app) {
        menu_button(menu, "Open new", all_action, 101);
        menu_button(menu, shell_app_pinned(catalog, app) ? "Unpin from dock" : "Pin to dock",
                    all_action, 100);
    }
    if (count) {
        menu_button(menu, "Show all windows", all_action, GXS_ACTIVATE);
        menu_button(menu, "Minimize all windows", all_action, GXS_MINIMIZE);
        menu_button(menu, "Close all windows", all_action, GXS_CLOSE);
    }
    regions();
    if (trace) printf("FUSEDE_WINDOWS app=%s count=%lu\n", id, (unsigned long)count);
}
static void app_launch(lv_event_t *e) {
    const ShellApp *app = lv_event_get_user_data(e);
    launch(app);
}
static int result_available(lv_obj_t *row) {
    return lv_obj_get_user_data(row) && lv_obj_has_state(row, LV_STATE_USER_1);
}
static void trace_selection(void) {
    const ShellApp *app = selected_row ? lv_obj_get_user_data(selected_row) : NULL;
    lv_area_t viewport, row;
    unsigned i, count = 0;
    int32_t index = -1;
    if (!trace || !menu_list) return;
    for (i = 0; i < lv_obj_get_child_count(menu_list); i++) {
        lv_obj_t *candidate = lv_obj_get_child(menu_list, i);
        if (result_available(candidate)) {
            if (candidate == selected_row) index = count;
            count++;
        }
    }
    lv_obj_get_coords(menu_list, &viewport);
    memset(&row, 0, sizeof(row));
    row.y2 = -1;
    if (selected_row) lv_obj_get_coords(selected_row, &row);
    printf("FUSEDE_SELECTION|%s|%ld|%u|%ld|%ld|%ld|%ld|%ld|%s\n", app ? app->id : "",
           (long)index, count, (long)(lv_obj_get_scroll_y(menu_list) / gx_lv_density()),
           (long)(row.y1 / gx_lv_density()),
           (long)((row.y2 - row.y1 + 1) / gx_lv_density()),
           (long)(viewport.y1 / gx_lv_density()),
           (long)((viewport.y2 - viewport.y1 + 1) / gx_lv_density()),
           search ? lv_textarea_get_text(search) : "");
}
static void select_result(lv_obj_t *row) {
    if (selected_row) lv_obj_remove_state(selected_row, LV_STATE_CHECKED);
    selected_row = row;
    if (selected_row) lv_obj_add_state(selected_row, LV_STATE_CHECKED);
    lv_obj_update_layout(lv_screen_active());
    if (selected_row) lv_obj_scroll_to_view(selected_row, LV_ANIM_OFF);
    lv_obj_update_layout(lv_screen_active());
    trace_selection();
}
static void navigate_results(int direction) {
    int32_t i, current = -1, count, visited;
    if (!menu_list) return;
    count = lv_obj_get_child_count(menu_list);
    for (i = 0; i < count; i++)
        if (lv_obj_get_child(menu_list, i) == selected_row) { current = i; break; }
    i = current < 0 ? (direction > 0 ? 0 : count - 1) : current + direction;
    for (visited = 0; visited < count; visited++, i += direction) {
        if (i < 0) i = count - 1;
        else if (i >= count) i = 0;
        lv_obj_t *row = lv_obj_get_child(menu_list, i);
        if (result_available(row)) { select_result(row); return; }
    }
    trace_selection();
}
static void populate_apps(void) {
    size_t i;
    unsigned matches = 0;
    lv_obj_t *first = NULL;
    const char *query = search ? lv_textarea_get_text(search) : "";
    selected_row = NULL;
    lv_obj_clean(menu_list);
    for (i = 0; i < shell_catalog_count(catalog); i++) {
        const ShellApp *a = shell_catalog_at(catalog, i);
        lv_obj_t *row, *labels, *name, *description;
        char detail[200];
        int available;
        if (category[0] && strcmp(category, a->category)) continue;
        if (!shell_app_matches(a, query)) continue;
        row = ui_button(menu_list, "", UI_ROW);
        /* A list row is an action; scrolling belongs to its outer viewport. */
        lv_obj_set_scrollable(row, false);
        /* ui_button creates an empty label; the shared row helper owns labels. */
        lv_obj_clean(row);
        gx_lv_width(row, menu_width - 160);
        gx_lv_height(row, 50);
        available = shell_app_available(catalog, a) != NULL;
        if (available) {
            lv_obj_add_state(row, LV_STATE_USER_1);
            if (!first) first = row;
        }
        snprintf(detail, sizeof(detail), "%s%s", a->kind == SHELL_APP_TERMINAL ? "Terminal application" : "Graphical application",
                 available ? "" : " · unavailable");
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        ui_icon(row, ui_icon_text(a->icon) ? a->icon : "file");
        labels = ui_stack(row, LV_FLEX_FLOW_COLUMN, 0);
        /* Decorative descendants must leave the row as the pointer target. */
        lv_obj_set_clickable(labels, false);
        lv_obj_set_width(labels, 0);
        lv_obj_set_flex_grow(labels, 1);
        name = ui_label(labels, a->name, UI_BODY);
        description = ui_label(labels, detail, UI_SECONDARY);
        lv_obj_set_width(name, lv_pct(100));
        lv_obj_set_width(description, lv_pct(100));
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_long_mode(description, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_user_data(row, (void *)a);
        lv_obj_add_event_cb(row, app_launch, LV_EVENT_CLICKED, (void *)a);
        matches++;
    }
    if (!matches) ui_label(menu_list, "No matching applications", UI_SECONDARY);
    if (menu_status) {
        char text[256];
        snprintf(text, sizeof(text), "%lu applications%s%s", (unsigned long)matches,
                 notice[0] ? " · " : "", notice);
        lv_label_set_text(menu_status, text);
    }
    lv_obj_scroll_to_y(menu_list, 0, LV_ANIM_OFF);
    select_result(first);
    if (trace) trace_menu(menu);
}
static void search_changed(lv_event_t *e) { (void)e; populate_apps(); }
static void select_category(lv_event_t *e) {
    const char *name = lv_event_get_user_data(e);
    snprintf(category, sizeof(category), "%s", name ? name : "");
    populate_apps();
    lv_group_focus_obj(search);
}
static void category_button(const char *name) {
    lv_obj_t *b = ui_button(category_list, name[0] ? name : "All apps", UI_ROW);
    gx_lv_width(b, 108);
    lv_obj_add_event_cb(b, select_category, LV_EVENT_CLICKED, (void *)name);
}
static void application_menu(void) {
    size_t i;
    const char *previous = "";
    lv_obj_t *body, *heading, *run;
    unsigned w = width > 600 ? 520 : width - 16;
    unsigned h = height > 640 ? 500 : height - 96;
    create_menu(1, w, h, (width - w) / 2, height);
    category[0] = 0;
    heading = ui_stack(menu, LV_FLEX_FLOW_ROW, 0);
    gx_lv_width(heading, menu_width - 26);
    gx_lv_height(heading, 30);
    lv_obj_set_flex_align(heading, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    ui_label(heading, "Applications", UI_HEADING);
    run = ui_button(heading, "Run…", UI_TOOL);
    gx_lv_size(run, 78, 28);
    lv_obj_add_event_cb(run, show_run, LV_EVENT_CLICKED, NULL);
    search = ui_input(menu, "Search applications…");
    gx_lv_width(search, menu_width - 26);
    lv_textarea_set_max_length(search, 128);
    lv_obj_add_event_cb(search, search_changed, LV_EVENT_VALUE_CHANGED, NULL);
    body = ui_stack(menu, LV_FLEX_FLOW_ROW, 8);
    gx_lv_width(body, menu_width - 26);
    lv_obj_set_height(body, 0);
    lv_obj_set_flex_grow(body, 1);
    category_list = ui_stack(body, LV_FLEX_FLOW_COLUMN, 2);
    gx_lv_width(category_list, 112);
    lv_obj_set_height(category_list, lv_pct(100));
    lv_obj_set_scrollable(category_list, true);
    category_button("");
    for (i = 0; i < shell_catalog_count(catalog); i++) {
        const ShellApp *a = shell_catalog_at(catalog, i);
        if (strcmp(previous, a->category)) {
            category_button(a->category);
            previous = a->category;
        }
    }
    menu_list = ui_stack(body, LV_FLEX_FLOW_COLUMN, 2);
    gx_lv_width(menu_list, menu_width - 146);
    lv_obj_set_height(menu_list, lv_pct(100));
    lv_obj_set_scrollable(menu_list, true);
    lv_obj_set_scroll_dir(menu_list, LV_DIR_VER);
    menu_status = ui_label(menu, "", UI_SECONDARY);
    gx_lv_width(menu_status, menu_width - 26);
    lv_label_set_long_mode(menu_status, LV_LABEL_LONG_MODE_DOTS);
    populate_apps();
    lv_group_focus_obj(search);
    regions();
    if (trace) puts("FUSEDE_APPLICATIONS");
}
static void run_geometry(void) {
    if (!menu || menu_kind != 5) return;
    lv_obj_set_height(menu, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(menu, gx_lv_px(height - 96), 0);
    lv_obj_update_layout(lv_screen_active());
    menu_height = (lv_obj_get_height(menu) + gx_lv_density() - 1) / gx_lv_density();
    menu_x = (width - menu_width) / 2;
    menu_y = (height - menu_height) / 2;
    gx_lv_pos(menu, menu_x, menu_y);
    lv_obj_update_layout(lv_screen_active());
    /* Group focus can queue a scroll while children are still being laid out.
     * The compact dialog fits its content; keep that obsolete animation from
     * moving the title and input out of view after the final measurement. */
    lv_obj_stop_scroll_anim(menu);
    lv_obj_scroll_to(menu, 0, 0, LV_ANIM_OFF);
    lv_obj_update_layout(lv_screen_active());
    if (trace) printf("FUSEDE_RUN_LAYOUT|%u|%u|error=%d\n", menu_width, menu_height,
                      run_status && !lv_obj_is_hidden(run_status));
}
static void run_error(const char *message) {
    if (run_status) {
        lv_label_set_text(run_status, message ? message : "");
        lv_obj_set_hidden(run_status, !message || !*message);
        run_geometry();
        regions();
    }
    if (trace && message && *message) printf("FUSEDE_RUN_ERROR|%s\n", message);
}
static void run_submit(void) {
    enum ShellRunMode mode;
    if (!run_input || run_pid > 0) return;
    snprintf(run_command, sizeof(run_command), "%s", lv_textarea_get_text(run_input));
    run_in_terminal = lv_obj_has_state(run_terminal, LV_STATE_CHECKED);
    mode = run_in_terminal ? SHELL_RUN_TERMINAL : SHELL_RUN_AUTO;
    if (!run_command[0]) {
        run_error("Enter a program name or path.");
        lv_group_focus_obj(run_input);
        return;
    }
    run_pid = shell_catalog_run(catalog, run_command, mode);
    if (run_pid < 0) {
        run_pid = 0;
        run_error(shell_catalog_error(catalog));
        lv_group_focus_obj(run_input);
        return;
    }
    lv_obj_add_state(run_button, LV_STATE_DISABLED);
    run_error(NULL);
    if (trace) printf("FUSEDE_RUN|%s|terminal=%d|pid=%u\n", run_command,
                      run_in_terminal, (unsigned)run_pid);
}
static void run_clicked(lv_event_t *event) { (void)event; run_submit(); }
static void run_cancel(lv_event_t *event) { (void)event; hide_menu(); }
static void run_dialog(void) {
    lv_obj_t *buttons, *cancel;
    unsigned w = width > 416 ? 400 : width - 16;
    create_menu(5, w, 160, (width - w) / 2, height / 2);
    lv_obj_set_style_pad_row(menu, gx_lv_px(UI_SPACE), 0);
    run_title = ui_label(menu, "Run", UI_HEADING);
    run_input = ui_input(menu, "Program or path");
    gx_lv_width(run_input, menu_width - 26);
    lv_textarea_set_max_length(run_input, 1024);
    lv_textarea_set_text(run_input, run_command);
    run_terminal = ui_checkbox(menu, "Run in terminal");
    if (run_in_terminal) lv_obj_add_state(run_terminal, LV_STATE_CHECKED);
    run_status = ui_label(menu, "", UI_ERROR);
    gx_lv_width(run_status, menu_width - 26);
    lv_label_set_long_mode(run_status, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_hidden(run_status, true);
    buttons = ui_stack(menu, LV_FLEX_FLOW_ROW, UI_SPACE);
    gx_lv_width(buttons, menu_width - 26);
    gx_lv_height(buttons, UI_CONTROL);
    lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    cancel = ui_button(buttons, "Cancel", UI_BUTTON);
    run_cancel_button = cancel;
    run_button = ui_button(buttons, "Run", UI_PRIMARY);
    gx_lv_width(cancel, 84);
    gx_lv_width(run_button, 84);
    lv_obj_add_event_cb(cancel, run_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(run_button, run_clicked, LV_EVENT_CLICKED, NULL);
    if (run_pid > 0) lv_obj_add_state(run_button, LV_STATE_DISABLED);
    run_geometry();
    lv_group_focus_obj(run_input);
    run_geometry();
    regions();
    if (trace) puts("FUSEDE_RUN_DIALOG");
}
static void show_run(lv_event_t *event) { (void)event; run_dialog(); }
static void toggle_applications(lv_event_t *e) {
    (void)e;
    if (menu_kind == 1) hide_menu(); else application_menu();
}
static void about_action(lv_event_t *e) {
    (void)e;
    create_menu(4, 330, 190, 12, 40);
    ui_label(menu, "FuseDE / Graphite", UI_HEADING);
    ui_label(menu, "A desktop for Riplix and Fuse-X.", UI_BODY);
    ui_label(menu, "Applications, windows and your workspace.", UI_SECONDARY);
    ui_label(menu, "Press Escape or click outside to close.", UI_SECONDARY);
    regions();
}
static void exit_action(lv_event_t *e) { (void)e; gx_lv_finish(); }
static void brand_clicked(lv_event_t *e) {
    (void)e;
    if (menu_kind == 3) { hide_menu(); return; }
    create_menu(3, 230, 158, 12, 42);
    ui_label(menu, "Riplix", UI_HEADING);
    menu_button(menu, "Applications", toggle_applications, 0);
    menu_button(menu, "About FuseDE", about_action, 0);
    menu_button(menu, "Exit desktop shell", exit_action, 0);
    regions();
}
static struct DockApp *add_app(const char *id, const char *name, const char *icon,
                               const ShellApp *entry) {
    struct DockApp *next = realloc(apps, (app_count + 1) * sizeof(*apps));
    struct DockApp *a;
    if (!next) return NULL;
    apps = next;
    a = &apps[app_count++];
    memset(a, 0, sizeof(*a));
    snprintf(a->id, sizeof(a->id), "%s", id);
    snprintf(a->name, sizeof(a->name), "%s", name);
    snprintf(a->icon, sizeof(a->icon), "%s", ui_icon_text(icon) ? icon : "file");
    a->catalog = entry;
    a->pinned = entry && shell_app_pinned(catalog, entry);
    return a;
}
static void refresh_dock(void) {
    size_t i, j, old_count = app_count;
    struct DockApp *old = apps;
    lv_obj_t *button, *icon;
    lv_obj_clean(dock);
    apps = NULL;
    app_count = 0;
    for (i = 0; i < shell_catalog_count(catalog); i++) {
        const ShellApp *a = shell_catalog_at(catalog, i);
        if (shell_app_pinned(catalog, a)) add_app(a->id, a->name, a->icon, a);
    }
    /* Preserve running-app order while older windows/instances disappear. */
    for (i = 0; i < old_count; i++) {
        size_t k;
        for (j = 0; j < app_count; j++) if (!strcmp(apps[j].id, old[i].id)) break;
        if (j < app_count) continue;
        for (k = 0; k < window_count; k++)
            if (same_app(&windows[k], old[i].id) && windows[k].window != gx_lv_window() &&
                !(windows[k].flags & GXS_HIDDEN) && windows[k].role == GXS_ROLE_NORMAL) {
                add_app(old[i].id, old[i].name, old[i].icon, old[i].catalog);
                break;
            }
    }
    free(old);
    for (i = 0; i < window_count; i++) {
        struct gx_session_record *w = &windows[i];
        char id[80];
        struct DockApp *a;
        const ShellApp *entry;
        if (w->window == gx_lv_window() || (w->flags & GXS_HIDDEN) || w->role != GXS_ROLE_NORMAL)
            continue;
        if (w->app_id[0]) snprintf(id, sizeof(id), "%s", w->app_id);
        else snprintf(id, sizeof(id), "window-process-%lu", (unsigned long)w->process);
        for (j = 0; j < app_count; j++) if (!strcmp(apps[j].id, id)) break;
        if (j == app_count) {
            entry = shell_catalog_find(catalog, id);
            a = add_app(id, entry ? entry->name : w->name[0] ? w->name : w->title,
                         entry ? entry->icon : w->icon, entry);
            if (!a) continue;
        } else a = &apps[j];
        a->windows++;
        if (w->flags & GXS_FOCUSED) a->focused = 1;
    }
    button = ui_icon_button(dock, "", "grid-2x2", UI_TOOL);
    lv_obj_clean(button);
    icon = ui_icon(button, "grid-2x2");
    lv_obj_center(icon);
    gx_lv_size(button, 48, 48);
    lv_obj_set_style_min_width(button, 0, 0);
    lv_obj_set_style_radius(button, gx_lv_px(10), 0);
    lv_obj_add_event_cb(button, toggle_applications, LV_EVENT_CLICKED, NULL);
    for (i = 0; i < app_count; i++) {
        lv_obj_t *dot;
        button = ui_button(dock, "", apps[i].focused ? UI_PRIMARY : UI_TOOL);
        lv_obj_clean(button);
        quiet(button);
        gx_lv_size(button, 48, 48);
        lv_obj_set_style_min_width(button, 0, 0);
        lv_obj_set_style_radius(button, gx_lv_px(10), 0);
        icon = ui_icon(button, apps[i].icon);
        lv_obj_center(icon);
        if (apps[i].windows) {
            dot = ui_swatch(button, apps[i].focused ? 0x9ab8efUL : 0x77808dUL);
            quiet(dot);
            /* The indicator decorates its button; it must not become a
             * separate hover or click target. Icons already follow this rule. */
            lv_obj_set_clickable(dot, false);
            lv_obj_set_click_focusable(dot, false);
            gx_lv_size(dot, 4, 4);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_align(dot, LV_ALIGN_BOTTOM_MID, 0, -gx_lv_px(2));
        }
        lv_obj_add_event_cb(button, app_activate, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        apps[i].button = button;
    }
    if (trace) printf("FUSEDE_DOCK apps=%lu windows=%lu\n", (unsigned long)app_count,
                      (unsigned long)window_count);
}
static void clock_update(void) {
    time_t now = time(NULL);
    struct tm *t;
    char text[80];
    if (now / 60 == clock_minute) return;
    clock_minute = now / 60;
    t = localtime(&now);
    if (t && t->tm_wday >= 0 && t->tm_wday < 7 && t->tm_mon >= 0 && t->tm_mon < 12) {
        static const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        /* The guest libc has localtime but no strftime yet. Match
         * strftime("%a %d %b %H:%M") without changing the shared libc ABI. */
        snprintf(text, sizeof(text), "%s %02d %s %02d:%02d", days[t->tm_wday],
                 t->tm_mday, months[t->tm_mon], t->tm_hour, t->tm_min);
        lv_label_set_text(clock_label, text);
        if (trace) printf("FUSEDE_CLOCK|%s\n", text);
    }
}
static void layout(void) {
    unsigned dw;
    /* Theme and density own the effective flex gaps. Measure the complete
     * content instead of duplicating that geometry in a guessed icon stride. */
    lv_obj_set_style_max_width(dock, gx_lv_px(width - 24), 0);
    lv_obj_set_width(dock, LV_SIZE_CONTENT);
    gx_lv_height(dock, 60);
    lv_obj_update_layout(lv_screen_active());
    dw = lv_obj_get_width(dock) / gx_lv_density();
    box(brand, 12, 8, 94, 30);
    box(clock_panel, width - (width < 400 ? 202 : 222), 8, width < 400 ? 190 : 210, 30);
    gx_lv_pos(dock, (width - dw) / 2, height - 68);
    if (menu_kind == 5) run_geometry();
    else if (menu) {
        if (menu_x + menu_width > width - 8) menu_x = width - 8 - menu_width;
        if (menu_y + menu_height > height - 80) menu_y = height - 80 - menu_height;
        if (menu_y < 40) menu_y = 40;
        box(menu, menu_x, menu_y, menu_width, menu_height);
    }
    regions();
    if (trace) {
        size_t i;
        printf("FUSEDE_LAYOUT dock=%u,%u,%u,60 menu=%d\n", (width-dw)/2, height-68, dw, menu_kind);
        for (i = 0; i < app_count; i++) {
            lv_area_t a;
            lv_obj_get_coords(apps[i].button, &a);
            printf("FUSEDE_ITEM %s %ld,%ld,%ld,%ld windows=%u pinned=%d\n", apps[i].id,
                   (long)(a.x1/gx_lv_density()), (long)(a.y1/gx_lv_density()),
                   (long)((a.x2-a.x1+1)/gx_lv_density()), (long)((a.y2-a.y1+1)/gx_lv_density()),
                   apps[i].windows, apps[i].pinned);
        }
    }
}
static void snapshot(void) {
    struct gx_session q;
    struct gx_session_record r, *next, *result = NULL;
    size_t count = 0;
    uint32_t after = 0, accepted = 0;
    unsigned attempts = 0;
restart:
    free(result);
    result = NULL;
    count = 0;
    after = accepted = 0;
    if (++attempts > 4) return;
    do {
        memset(&q, 0, sizeof(q));
        q.version = GXS_VERSION;
        q.operation = GXS_NEXT_WINDOW;
        q.object = after;
        q.revision = accepted;
        q.record = &r;
        if (ioctl(gx_lv_fd(), GX_SESSION, &q) < 0) {
            if (errno == EAGAIN) goto restart;
            free(result);
            return;
        }
        accepted = q.revision;
        after = q.object;
        if (after) {
            next = realloc(result, (count + 1) * sizeof(*result));
            if (!next) { free(result); return; }
            result = next;
            result[count++] = r;
        }
    } while (after);
    free(windows);
    windows = result;
    window_count = count;
    revision = accepted;
    refresh_dock();
    layout();
    memset(&q, 0, sizeof(q));
    q.version = GXS_VERSION;
    q.operation = GXS_WATCH;
    q.revision = revision;
    ioctl(gx_lv_fd(), GX_SESSION, &q);
}
static void idle(void) {
    struct gx_session q;
    ShellLaunchEvent e;
    memset(&q, 0, sizeof(q));
    q.version = GXS_VERSION;
    q.operation = GXS_WATCH;
    q.revision = revision;
    if (!revision || pending_refresh ||
        (ioctl(gx_lv_fd(), GX_SESSION, &q) == 0 && (q.flags & GXS_CHANGED))) {
        pending_refresh = 0;
        snapshot();
    }
    while (shell_catalog_poll(catalog, &e) > 0) {
        int from_run = run_pid > 0 && e.pid == run_pid;
        if (from_run && (e.state == SHELL_LAUNCH_STARTED || e.state == SHELL_LAUNCH_FAILED)) {
            run_pid = 0;
            if (e.state == SHELL_LAUNCH_STARTED) {
                if (menu_kind == 5) hide_menu();
            } else if (menu_kind == 5) {
                char failure[200];
                snprintf(failure, sizeof(failure), "Cannot start program: %s", strerror(e.error));
                run_error(failure);
                lv_obj_remove_state(run_button, LV_STATE_DISABLED);
                lv_group_focus_obj(run_input);
            }
        }
        if (!from_run && e.state == SHELL_LAUNCH_FAILED) {
            char failure[160];
            snprintf(failure, sizeof(failure), "%s (%s)", e.app_id, strerror(e.error));
            shell_error("Cannot launch", failure);
            if (!menu) application_menu();
        }
        if (trace) printf("FUSEDE_LAUNCH app=%s state=%u pid=%u error=%d\n",
                          e.app_id, e.state, (unsigned)e.pid, e.error);
    }
    clock_update();
}
static void text(uint32_t scalar) {
    char b[5];
    lv_obj_t *input = run_input ? run_input : search;
    if (!input || (run_input && (run_pid > 0 || lv_group_get_focused(gx_lv_group()) != run_input)) || scalar < 32 || scalar > 0x10ffffUL) return;
    if (scalar < 128) { b[0] = scalar; b[1] = 0; }
    else if (scalar < 2048) { b[0] = 192 | (scalar >> 6); b[1] = 128 | (scalar & 63); b[2] = 0; }
    else if (scalar < 65536UL) { b[0] = 224 | (scalar >> 12); b[1] = 128 | ((scalar >> 6) & 63); b[2] = 128 | (scalar & 63); b[3] = 0; }
    else { b[0] = 240 | (scalar >> 18); b[1] = 128 | ((scalar >> 12) & 63); b[2] = 128 | ((scalar >> 6) & 63); b[3] = 128 | (scalar & 63); b[4] = 0; }
    if (replace_input == input) {
        lv_textarea_set_text(input, "");
        replace_input = NULL;
    }
    lv_textarea_add_text(input, b);
}
static void field_action(unsigned code) {
    lv_obj_t *input = run_input ? run_input : search;
    char buffer[4097];
    const char *value;
    if (!input || (run_input && run_pid > 0) || lv_group_get_focused(gx_lv_group()) != input)
        return;
    value = lv_textarea_get_text(input);
    if (code == GX_ACTION_SELECT_ALL) {
        const unsigned char *s = (const unsigned char *)value;
        unsigned count = 0;
        for (; *s; s++) if ((*s & 192) != 128) count++;
        lv_label_set_text_selection_start(lv_textarea_get_label(input), 0);
        lv_label_set_text_selection_end(lv_textarea_get_label(input), count);
        replace_input = input;
    } else if (code == GX_ACTION_PASTE) {
        if (!gx_lv_clipboard(buffer, sizeof(buffer), 0) && !strpbrk(buffer, "\r\n")) {
            if (replace_input == input) lv_textarea_set_text(input, "");
            replace_input = NULL;
            lv_textarea_add_text(input, buffer);
        } else if (run_input) run_error("Clipboard text is too long or contains a line break.");
    } else if (code == GX_ACTION_COPY || code == GX_ACTION_CUT) {
        if (strlen(value) < sizeof(buffer)) {
            strcpy(buffer, value);
            if (!gx_lv_clipboard(buffer, sizeof(buffer), 1) && code == GX_ACTION_CUT) {
                lv_textarea_set_text(input, "");
                replace_input = NULL;
            }
        }
    }
}
static int raw_event(const struct gx_event *e) {
    size_t i;
    lv_obj_t *input = run_input ? run_input : search;
    if (replace_input && e->type == GX_BUTTON && (e->buttons & 1)) {
        lv_textarea_clear_selection(replace_input);
        replace_input = NULL;
    }
    if (input && replace_input == input && e->type == GX_KEY && (e->flags & GX_DOWN)) {
        if (e->code == 42 || e->code == 76) {
            lv_textarea_set_text(input, "");
            replace_input = NULL;
            return 1;
        }
        if (e->code == 79 || e->code == 80 || e->code == 74 || e->code == 77) {
            lv_textarea_clear_selection(input);
            replace_input = NULL;
        }
    }
    if (menu_kind == 1 && search && lv_group_get_focused(gx_lv_group()) == search &&
        e->type == GX_KEY) {
        if (e->code == 81 || e->code == 82) {
            if (e->flags & GX_DOWN) navigate_results(e->code == 81 ? 1 : -1);
            return 1;
        }
        if (e->code == 40 || e->code == 88) {
            if ((e->flags & GX_DOWN) && selected_row && result_available(selected_row))
                launch(lv_obj_get_user_data(selected_row));
            return 1;
        }
    }
    if (menu_kind == 5 && e->type == GX_KEY) {
        if (e->code == 40 || e->code == 88) {
            if (e->flags & GX_DOWN) {
                if (lv_group_get_focused(gx_lv_group()) == run_cancel_button) hide_menu();
                else run_submit();
            }
            return 1;
        }
        if (e->code == 44 && lv_group_get_focused(gx_lv_group()) == run_terminal) {
            if (e->flags & GX_DOWN) {
                if (lv_obj_has_state(run_terminal, LV_STATE_CHECKED))
                    lv_obj_remove_state(run_terminal, LV_STATE_CHECKED);
                else lv_obj_add_state(run_terminal, LV_STATE_CHECKED);
            }
            return 1;
        }
    }
    if (e->type == GX_KEY && e->code == 41 && (e->flags & GX_DOWN)) {
        hide_menu();
        return 1;
    }
    if (e->type == GX_BUTTON && ((e->buttons & 2) || right_pressed)) {
        if (e->buttons & 2) {
            if (!right_pressed) {
                if (menu_kind == 1 && menu_list && inside(e->x, e->y, menu_list)) {
                    unsigned n;
                    for (n = 0; n < lv_obj_get_child_count(menu_list); n++) {
                        lv_obj_t *row = lv_obj_get_child(menu_list, n);
                        const ShellApp *a = lv_obj_get_user_data(row);
                        if (a && inside(e->x, e->y, row)) {
                            window_menu(a->id);
                            break;
                        }
                    }
                } else for (i = 0; i < app_count; i++)
                    if (inside(e->x, e->y, apps[i].button)) {
                        window_menu(apps[i].id);
                        break;
                    }
            }
            right_pressed = 1;
        } else right_pressed = 0;
        return 1;
    }
    if (menu && e->type == GX_BUTTON && (e->buttons & 1) &&
        !inside(e->x, e->y, menu) && !inside(e->x, e->y, brand) &&
        !inside(e->x, e->y, dock)) {
        hide_menu();
        return 1;
    }
    return 0;
}
static void resize(unsigned w, unsigned h) {
    width = w;
    height = h;
    /* Menus can be reopened at their new safe geometry, without stale captures. */
    hide_menu();
    refresh_dock();
    layout();
}
static int prepare_pins(void) {
    const char *override = getenv("FUSEDE_PINS"), *home = getenv("HOME");
    char parent[512];
    if (override) { snprintf(pins_path, sizeof(pins_path), "%s", override); return 0; }
    if (!home || !*home) home = "/root";
    if (strlen(home) + 32 >= sizeof(pins_path)) return -1;
    snprintf(parent, sizeof(parent), "%s/.config", home);
    if (mkdir(parent, 0700) < 0 && errno != EEXIST) return -1;
    snprintf(parent, sizeof(parent), "%s/.config/fusede", home);
    if (mkdir(parent, 0700) < 0 && errno != EEXIST) return -1;
    snprintf(pins_path, sizeof(pins_path), "%s/pins.ini", parent);
    return 0;
}
int main(int argc, char **argv) {
    struct gx_session q;
    int probe, status;
    const char *directory = getenv("FUSEDE_APPLICATIONS");
    trace = argc > 1 && !strcmp(argv[1], "--trace");
    if (argc > 1 && !trace) { puts("Usage: fusedeshell [--trace]"); return !strcmp(argv[1], "--help") ? 0 : 1; }
    catalog = shell_catalog_new();
    if (!catalog) return 1;
    if (!directory) directory = "/usr/share/applications";
    if (shell_catalog_load(catalog, directory) < 0)
        shell_error("Applications", shell_catalog_error(catalog));
    shell_catalog_terminal(catalog, "/bin/gterm", "/mnt/host/gterm.fxe");
    if (prepare_pins() < 0) shell_error("Pin configuration", strerror(errno));
    else if (shell_pins_load(catalog, pins_path) < 0) shell_error("Pins", shell_catalog_error(catalog));
    probe = open("/dev/gfx", O_RDWR | O_CLOEXEC);
    memset(&q, 0, sizeof(q));
    q.version = GXS_VERSION;
    q.operation = GXS_CLAIM_MANAGER;
    if (probe < 0 || ioctl(probe, GX_SESSION, &q) < 0) {
        fputs("FuseDE: desktop manager authority unavailable\n", stderr);
        if (probe >= 0) close(probe);
        shell_catalog_free(catalog);
        return 1;
    }
    authority = q.authority;
    close(probe);
    gx_lv_application("org.riplix.FuseDE", "FuseDE", "grid-2x2");
    if (gx_lv_open_panel("FuseDE", authority, &width, &height) < 0) {
        fputs("FuseDE: this display does not support desktop panels\n", stderr);
        shell_catalog_free(catalog);
        return 1;
    }
    ui_screen();
    quiet(lv_screen_active());
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x15191cUL), 0);
    if (gx_lv_panel_alpha()) {
        lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_opa(lv_screen_active(), LV_OPA_TRANSP, 0);
        lv_obj_set_style_outline_opa(lv_screen_active(), LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_opa(lv_screen_active(), LV_OPA_TRANSP, 0);
    }
    brand = ui_button(lv_screen_active(), "Riplix", UI_TOOL);
    lv_obj_set_style_radius(brand, gx_lv_px(12), 0);
    lv_obj_add_event_cb(brand, brand_clicked, LV_EVENT_CLICKED, NULL);
    clock_panel = ui_panel(lv_screen_active());
    quiet(clock_panel);
    lv_obj_set_style_radius(clock_panel, gx_lv_px(12), 0);
    clock_label = ui_label(clock_panel, "", UI_BODY);
    lv_obj_center(clock_label);
    dock = ui_panel(lv_screen_active());
    lv_obj_set_style_radius(dock, gx_lv_px(16), 0);
    lv_obj_set_style_pad_all(dock, gx_lv_px(6), 0);
    lv_obj_set_style_pad_column(dock, gx_lv_px(6), 0);
    lv_obj_set_flex_flow(dock, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dock, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(dock, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(dock, LV_SCROLLBAR_MODE_OFF);
    refresh_dock();
    clock_update();
    layout();
    gx_lv_handlers(text, field_action, resize);
    gx_lv_events(raw_event);
    gx_lv_idle(idle);
    if (trace) printf("FUSEDE_READY width=%u height=%u catalog=%lu\n", width, height,
                      (unsigned long)shell_catalog_count(catalog));
    status = gx_lv_run();
    hide_menu();
    gx_lv_close();
    free(apps);
    free(windows);
    shell_catalog_free(catalog);
    return status;
}
