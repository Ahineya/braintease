/* SPDX-License-Identifier: MIT */
#include "ui-app.h"
#include "csv.h"
#include "data-plot.h"
#include <gx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#define PATH_SIZE 512
#define RECENTS 8
struct preferences {
    unsigned header, delimiter, widths[CSV_MAX_COLUMNS], count, details, split;
    unsigned plot, plot_split, plot_x, plot_y, plot_mode;
    char (*recent)[PATH_SIZE];
};
static struct preferences prefs;
static struct csv_document document;
static ui_table *table;
static ui_actions *actions;
static ui_split *workspace;
static ui_split *plot_split;
static data_plot *plots;
static ui_progress *progress;
static struct csv_job *csv_work;
static uint32_t sort_source, restore_row, original_row, *original_order;
static unsigned work_kind, sort_column_number, sort_descending, selected_column, work_cancelled,
    work_focus;
enum { WORK_LOAD = 1, WORK_SORT, WORK_RESTORE, WORK_ORIGINAL };
static lv_obj_t *inspector_meta, *inspector_value, *inspector_hint;
static lv_obj_t *menubar, *toolbar, *title, *summary, *status, *detail, *file_menu, *view_menu,
    *help_menu;
static lv_obj_t *option_header, *option_delimiter, *option_error;
static ui_path *open_dialog;
static lv_timer_t *work_timer;
static struct file_paths {
    char current[PATH_SIZE], pending[PATH_SIZE], settings[PATH_SIZE], error[200];
} *paths;
#define current_path paths->current
#define pending_path paths->pending
#define settings_path paths->settings
static unsigned pending_delimiter, pending_header, option_choice;
static int trace, busy, save_warning;
static unsigned screen_width = 800, screen_height = 560;
static const unsigned delimiters[] = {0, ',', '\t', ';'};
static const char *delimiter_names[] = {"Automatic", "Comma", "Tab", "Semicolon"};
enum {
    OPEN = 1,
    RELOAD,
    OPTIONS,
    COPY_CELL,
    COPY_ROW,
    CLOSE,
    ORIGINAL,
    WIDER,
    NARROWER,
    RESET_WIDTHS,
    ABOUT,
    CLEAR_RECENT,
    DETAILS,
    PLOT,
    EXPORT_PLOT,
    RECENT_FIRST = 100
};
static const struct ui_action_spec commands[] = {
    {OPEN, "Open", "Ctrl+O", "folder", 18, 1, 1},
    {RELOAD, "Reload", "F5", "refresh-cw", 62, 0, 0},
    {OPTIONS, "Options", "F8", "settings", 65, 0, 1},
    {COPY_CELL, "Copy cell", "Ctrl+C", "file", 6, 1, 0},
    {COPY_ROW, "Copy row", "Ctrl+Shift+C", "grid-2x2", 6, 3, 0},
    {CLOSE, "Close", "Ctrl+W", "x", 26, 1, 1},
    {ORIGINAL, "Original row order", "", "grid-2x2", 0, 0, 0},
    {WIDER, "Wider column", "", "arrow-right", 0, 0, 0},
    {NARROWER, "Narrower column", "", "arrow-left", 0, 0, 0},
    {RESET_WIDTHS, "Reset column widths", "", "refresh-cw", 0, 0, 1},
    {ABOUT, "About Data Viewer", "F1", "info", 58, 0, 1},
    {CLEAR_RECENT, "Clear recent files", "", "trash", 0, 0, 0},
    {DETAILS, "Details pane", "Ctrl+D", "file", 7, 1, 1},
    {PLOT, "Plot", "Ctrl+P", "grid-2x2", 19, 1, 1},
    {EXPORT_PLOT, "Export plot", "Ctrl+E", "save", 8, 1, 0}
};
static void update_actions(void) {
    unsigned i;
    for (i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        unsigned id = commands[i].id;
        int enabled = !busy || id == CLOSE;
        if (id == RELOAD)
            enabled = enabled && current_path[0];
        if (id == COPY_CELL || id == COPY_ROW || id == ORIGINAL || id == WIDER || id == NARROWER)
            enabled = enabled && document.row_count;
        if (id == CLEAR_RECENT)
            enabled = enabled && prefs.count;
        if (id == EXPORT_PLOT)
            enabled = enabled && data_plot_can_export(plots);
        ui_actions_enable(actions, id, enabled);
    }
    if (plots)
        data_plot_enabled(plots, !busy);
    if (table)
        ui_table_enabled(table, !busy);
    if (menubar) {
        if (busy) {
            lv_obj_add_state(file_menu, LV_STATE_DISABLED);
            lv_obj_add_state(view_menu, LV_STATE_DISABLED);
            lv_obj_add_state(help_menu, LV_STATE_DISABLED);
        } else {
            lv_obj_remove_state(file_menu, LV_STATE_DISABLED);
            lv_obj_remove_state(view_menu, LV_STATE_DISABLED);
            lv_obj_remove_state(help_menu, LV_STATE_DISABLED);
        }
    }
}
static void plot_changed(void *unused) {
    (void)unused;
    update_actions();
}

static const char *basename_of(const char *path) {
    const char *last = strrchr(path, '/');
    return last ? last + 1 : path;
}
static void tell(const char *message) {
    lv_label_set_text(status, message);
    if (trace)
        fprintf(stderr, "DATA_STATUS %s\n", message);
}
static void settings_load(void) {
    const char *home = getenv("HOME");
    FILE *f;
    char line[2100];
    unsigned i;
    prefs.header = 1;
    prefs.split = 700;
    prefs.plot_split = 450;
    prefs.plot_y = CSV_MAX_COLUMNS;
    for (i = 0; i < CSV_MAX_COLUMNS; i++)
        prefs.widths[i] = 144;
    if (!home || !*home || strlen(home) + 10 >= sizeof(settings_path))
        return;
    snprintf(settings_path, sizeof(settings_path), "%s/.lvdata", home);
    f = fopen(settings_path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        char *end = strpbrk(line, "\r\n");
        if (end)
            *end = 0;
        if (!strncmp(line, "header=", 7))
            prefs.header = atoi(line + 7) == 1;
        else if (!strncmp(line, "details=", 8))
            prefs.details = atoi(line + 8) == 1;
        else if (!strncmp(line, "plot=", 5))
            prefs.plot = atoi(line + 5) == 1;
        else if (!strncmp(line, "plot_split=", 11)) {
            unsigned n = strtoul(line + 11, NULL, 10);
            if (n <= 1000)
                prefs.plot_split = n;
        } else if (!strncmp(line, "plot_x=", 7)) {
            unsigned n = strtoul(line + 7, NULL, 10);
            if (n <= CSV_MAX_COLUMNS)
                prefs.plot_x = n;
        } else if (!strncmp(line, "plot_y=", 7)) {
            unsigned n = strtoul(line + 7, NULL, 10);
            if (n <= CSV_MAX_COLUMNS)
                prefs.plot_y = n;
        } else if (!strncmp(line, "plot_mode=", 10))
            prefs.plot_mode = atoi(line + 10) == 1;
        else if (!strncmp(line, "split=", 6)) {
            unsigned n = strtoul(line + 6, NULL, 10);
            if (n <= 1000)
                prefs.split = n;
        } else if (!strncmp(line, "delimiter=", 10)) {
            unsigned n = atoi(line + 10);
            if (n == 0 || n == ',' || n == '\t' || n == ';')
                prefs.delimiter = n;
        } else if (!strncmp(line, "recent=", 7) && prefs.count < RECENTS && line[7] == '/' &&
                   strlen(line + 7) < PATH_SIZE)
            strcpy(prefs.recent[prefs.count++], line + 7);
        else if (!strncmp(line, "widths=", 7)) {
            char *s = line + 7;
            for (i = 0; i < CSV_MAX_COLUMNS && *s; i++) {
                char *next;
                unsigned n = strtoul(s, &next, 10);
                if (next == s)
                    break;
                if (n >= 64 && n <= 480)
                    prefs.widths[i] = n;
                s = *next == ',' ? next + 1 : next;
            }
        }
    }
    fclose(f);
}
static int settings_save(void) {
    char temporary[PATH_SIZE + 8];
    FILE *f;
    unsigned i;
    int bad;
    if (!settings_path[0])
        return 0;
    if (table)
        for (i = 0; i < CSV_MAX_COLUMNS; i++)
            prefs.widths[i] = ui_table_get_width(table, i);
    if (plots)
        data_plot_options(plots, &prefs.plot_x, &prefs.plot_y, &prefs.plot_mode);
    snprintf(temporary, sizeof(temporary), "%s.new", settings_path);
    f = fopen(temporary, "w");
    if (!f)
        return -1;
    fprintf(f, "version=1\nheader=%u\ndelimiter=%u\ndetails=%u\nsplit=%u\nwidths=",
            prefs.header, prefs.delimiter, prefs.details, prefs.split);
    for (i = 0; i < CSV_MAX_COLUMNS; i++)
        fprintf(f, "%s%u", i ? "," : "", prefs.widths[i]);
    fputc('\n', f);
    fprintf(f, "plot=%u\nplot_split=%u\nplot_x=%u\nplot_y=%u\nplot_mode=%u\n",
            prefs.plot, prefs.plot_split, prefs.plot_x, prefs.plot_y, prefs.plot_mode);
    for (i = 0; i < prefs.count; i++)
        fprintf(f, "recent=%s\n", prefs.recent[i]);
    bad = ferror(f);
    if (fclose(f))
        bad = 1;
    if (bad || rename(temporary, settings_path)) {
        unlink(temporary);
        return -1;
    }
    return 0;
}
static void recent_add(const char *path) {
    unsigned i, j;
    for (i = 0; i < prefs.count; i++)
        if (!strcmp(path, prefs.recent[i]))
            break;
    if (i == prefs.count && prefs.count < RECENTS)
        prefs.count++;
    if (i >= RECENTS)
        i = RECENTS - 1;
    for (j = i; j; j--)
        strcpy(prefs.recent[j], prefs.recent[j - 1]);
    strcpy(prefs.recent[0], path);
}
static const char *value(void *unused, uint32_t row, unsigned column) {
    (void)unused;
    return csv_cell(&document, row, column);
}
static void inspect_cell(void) {
    char *preview, caption[120];
    const char *s;
    unsigned at = 0;
    if (!ui_split_second_visible(workspace))
        return;
    if (!document.row_count) {
        lv_label_set_text(inspector_meta, "No cell selected");
        lv_label_set_text(inspector_value, "Open a data file to inspect a cell.");
        lv_label_set_text(inspector_hint, "");
        return;
    }
    preview = malloc(4096);
    if (!preview) {
        lv_label_set_text(inspector_value, "Not enough memory to show this cell.");
        lv_label_set_text(inspector_hint, "");
        return;
    }
    snprintf(caption, sizeof(caption), "Row %lu  |  Column %u",
             (unsigned long)ui_table_row(table) + 1, ui_table_column(table) + 1);
    lv_label_set_text(inspector_meta, caption);
    s = csv_cell(&document, ui_table_row(table), ui_table_column(table));
    while (*s && at + 5 < 4096) {
        unsigned c = (unsigned char)*s, n = c < 128 ? 1 : c < 224 ? 2 : c < 240 ? 3 : 4, i;
        if (c < 32 && c != '\n' && c != '\t') {
            preview[at++] = ' ';
            s++;
        } else
            for (i = 0; i < n && *s; i++)
                preview[at++] = *s++;
    }
    preview[at] = 0;
    lv_label_set_text(inspector_value, preview);
    lv_label_set_text(inspector_hint, *s ? "Preview limited to 4 KiB."
                                       : "");
    free(preview);
}
static void split_changed(lv_event_t *event) {
    (void)event;
    prefs.split = ui_split_get_position(workspace);
    prefs.details = ui_split_second_visible(workspace);
    if (trace)
        fprintf(stderr, "DATA_SPLIT %u visible=%u\n", prefs.split, prefs.details);
}
static void plot_split_changed(lv_event_t *event) {
    (void)event;
    prefs.plot_split = ui_split_get_position(plot_split);
    prefs.plot = ui_split_second_visible(plot_split);
    if (trace)
        fprintf(stderr, "DATA_PLOT_SPLIT %u visible=%u\n", prefs.plot_split, prefs.plot);
}
static void selection(void *unused, uint32_t row, unsigned column) {
    char text[200];
    const char *s = csv_cell(&document, row, column);
    unsigned at = 0;
    (void)unused;
    snprintf(text, sizeof(text), "Row %lu of %lu  |  Column %u of %u%s", (unsigned long)row + 1,
             (unsigned long)document.row_count, column + 1, document.columns,
             save_warning ? "  |  Settings could not be saved" : "");
    tell(text);
    while (*s && at + 5 < sizeof(text)) {
        unsigned c = (unsigned char)*s, n = c < 128 ? 1 : c < 224 ? 2 : c < 240 ? 3 : 4, i;
        if (at + n + 4 >= sizeof(text))
            break;
        if (c < 32 || c == 127) {
            text[at++] = ' ';
            s++;
            continue;
        }
        for (i = 0; i < n && *s; i++)
            text[at++] = *s++;
    }
    if (*s) {
        text[at++] = '.';
        text[at++] = '.';
        text[at++] = '.';
    }
    text[at] = 0;
    lv_label_set_text(detail, text);
    inspect_cell();
    if (plots)
        data_plot_select(plots, row);
    if (trace)
        fprintf(stderr, "DATA_SELECT %lu %u\n", (unsigned long)row, column);
}
static void bind_document(void) {
    unsigned i;
    char text[120];
    if (plots)
        data_plot_document(plots);
    for (i = 0; i < document.columns; i++)
        ui_table_heading(table, i, csv_heading(&document, i));
    ui_table_bind(table, document.row_count, document.columns);
    ui_table_sort_marker(table, document.sort_column, document.descending);
    lv_label_set_text(title, basename_of(current_path));
    snprintf(text, sizeof(text), "%lu rows  |  %u columns", (unsigned long)document.row_count,
             document.columns);
    lv_label_set_text(summary, text);
    lv_obj_update_layout(ui_table_object(table));
    ui_table_density(table);
    if (document.row_count)
        ui_table_select(table, 0, 0, 1);
    else {
        tell("No data rows");
        lv_label_set_text(detail, "");
    }
    ui_table_focus(table);
    inspect_cell();
    update_actions();
}
static void work_stop(void) {
    if (work_timer) {
        lv_timer_delete(work_timer);
        work_timer = NULL;
    }
    csv_job_free(csv_work);
    csv_work = NULL;
    free(original_order);
    original_order = NULL;
    busy = work_kind = work_cancelled = work_focus = 0;
    if (progress) {
        ui_progress_running(progress, 0);
        lv_obj_set_hidden(ui_progress_object(progress), true);
    }
}
static void work_cancel(ui_progress *widget, void *unused) {
    (void)widget;
    (void)unused;
    work_cancelled = 1;
}
static void work_tick(lv_timer_t *timer) {
    uint32_t start = lv_tick_get();
    unsigned rounds = 0;
    int result = CSV_JOB_RUNNING;
    (void)timer;
    /* Open/Options callbacks start the job while their modal still exists.
     * Focus after it closes, so modal teardown cannot restore focus over Cancel. */
    if (work_focus && !ui_overlay_active()) {
        ui_progress_focus_cancel(progress);
        work_focus = 0;
    }
    if (work_cancelled && work_kind != WORK_RESTORE) {
        if (csv_work)
            csv_job_cancel(csv_work);
        work_stop();
        update_actions();
        tell("Cancelled. The current document was kept.");
        ui_table_focus(table);
        if (trace)
            fprintf(stderr, "DATA_CANCELLED\n");
        return;
    }
    do {
        if (work_kind == WORK_RESTORE) {
            unsigned count = 0;
            while (restore_row < document.row_count && count++ < 4096 &&
                   document.order[restore_row] != sort_source)
                restore_row++;
            if (restore_row == document.row_count || document.order[restore_row] == sort_source) {
                uint32_t row = restore_row < document.row_count ? restore_row : 0;
                work_stop();
                ui_table_sort_marker(table, document.sort_column, document.descending);
                data_plot_document(plots);
                update_actions();
                ui_table_select(table, row, selected_column, 1);
                ui_table_focus(table);
                if (trace)
                    fprintf(stderr, "DATA_SORT %u %u first=%s\n", sort_column_number,
                            sort_descending, csv_cell(&document, 0, sort_column_number));
                return;
            }
        } else if (work_kind == WORK_ORIGINAL) {
            unsigned count = 0;
            uint32_t offset = document.header && document.record_count ? 1 : 0;
            while (original_row < document.row_count && count++ < 4096) {
                original_order[original_row] = original_row + offset;
                original_row++;
            }
            if (original_row == document.row_count) {
                free(document.order);
                document.order = original_order;
                original_order = NULL;
                document.sort_column = -1;
                document.descending = 0;
                work_kind = WORK_RESTORE;
                ui_progress_running(progress, 0);
                restore_row = 0;
            }
        } else {
            result = csv_job_step(csv_work, 4096);
            if (result == CSV_JOB_ERROR || result == CSV_JOB_CANCELLED) {
                char error[200];
                snprintf(error, sizeof(error), "%s", csv_job_error(csv_work));
                work_stop();
                update_actions();
                ui_table_focus(table);
                tell("The current document was kept.");
                ui_message("Cannot process file", error);
                if (trace)
                    fprintf(stderr, "DATA_ERROR %s\n", error);
                return;
            }
            if (result == CSV_JOB_DONE) {
                if (work_kind == WORK_LOAD) {
                    strcpy(current_path, pending_path);
                    recent_add(current_path);
                    work_stop();
                    save_warning = settings_save() < 0;
                    bind_document();
                    if (trace)
                        fprintf(stderr, "DATA_OPEN %s rows=%lu columns=%u delimiter=%u widgets=%u\n",
                                current_path, (unsigned long)document.row_count, document.columns,
                                document.delimiter, ui_table_widget_count(table));
                    return;
                }
                csv_job_free(csv_work);
                csv_work = NULL;
                work_kind = WORK_RESTORE;
                ui_progress_running(progress, 0);
                restore_row = 0;
            }
        }
    } while (++rounds < 16 && lv_tick_elaps(start) < 4);
    if (csv_work)
        ui_progress_update(progress, csv_job_phase(csv_work), csv_job_progress(csv_work), 1000);
    else
        ui_progress_update(progress, work_kind == WORK_ORIGINAL ? "Restoring row order..."
                                                              : "Restoring selection...",
                           work_kind == WORK_ORIGINAL ? original_row : restore_row,
                           document.row_count);
}
static int work_begin(unsigned kind) {
    data_plot_cancel(plots);
    busy = 1;
    work_cancelled = 0;
    work_focus = 1;
    work_kind = kind;
    ui_progress_running(progress, 1);
    ui_progress_update(progress, kind == WORK_LOAD ? "Opening file..." : "Sorting...", 0, 1000);
    lv_obj_set_hidden(ui_progress_object(progress), false);
    update_actions();
    if (!ui_overlay_active()) {
        ui_progress_focus_cancel(progress);
        work_focus = 0;
    }
    work_timer = lv_timer_create(work_tick, 5, NULL);
    if (!work_timer) {
        work_stop();
        update_actions();
        snprintf(paths->error, sizeof(paths->error), "Could not start the operation. Try again.");
        return 0;
    }
    if (trace)
        fprintf(stderr, "DATA_WORK_START %u\n", kind);
    return 1;
}
static int absolute_path(const char *path, char *out) {
    char cwd[PATH_SIZE];
    if (!path[0] || strpbrk(path, "\r\n"))
        return -1;
    if (path[0] == '/') {
        if (strlen(path) >= PATH_SIZE)
            return -1;
        strcpy(out, path);
    } else {
        if (!getcwd(cwd, sizeof(cwd)) || strlen(cwd) + strlen(path) + 2 > PATH_SIZE)
            return -1;
        snprintf(out, PATH_SIZE, "%s/%s", cwd, path);
    }
    return 0;
}
static int queue_load(const char *path) {
    paths->error[0] = 0;
    if (busy) {
        snprintf(paths->error, sizeof(paths->error), "Wait for the current operation to finish.");
        return 0;
    }
    if (!path || absolute_path(path, pending_path)) {
        snprintf(paths->error, sizeof(paths->error), "Enter a file path shorter than 512 characters.");
        return 0;
    }
    pending_delimiter = prefs.delimiter;
    pending_header = prefs.header;
    csv_work = csv_load_begin(&document, pending_path, pending_delimiter, pending_header,
                              paths->error, sizeof(paths->error));
    if (!csv_work) {
        if (trace)
            fprintf(stderr, "DATA_ERROR %s\n", paths->error);
        return 0;
    }
    return work_begin(WORK_LOAD);
}
static int open_result(ui_path *dialog, const char *path, void *unused) {
    (void)dialog;
    (void)unused;
    if (!path) {
        open_dialog = NULL;
        return 1;
    }
    if (!queue_load(path)) {
        ui_path_error(dialog, paths->error[0] ? paths->error : "Enter a file path shorter than 512 characters.");
        return 0;
    }
    open_dialog = NULL;
    return 1;
}
static void open_file(void) {
    if (busy || ui_overlay_active())
        return;
    open_dialog = ui_path_open("Open data file", UI_PATH_OPEN, current_path, open_result, NULL);
    if (!open_dialog)
        return;
    if (trace) {
        fprintf(stderr, "DATA_DIALOG open\n");
        if (ui_path_list(open_dialog)) {
            lv_area_t area;
            lv_obj_update_layout(lv_layer_top());
            lv_obj_get_coords(ui_table_object(ui_path_list(open_dialog)), &area);
            fprintf(stderr, "DATA_PATH_LIST %ld,%ld %ldx%ld density=%lu\n",
                    (long)area.x1, (long)area.y1, (long)(area.x2-area.x1+1),
                    (long)(area.y2-area.y1+1), (unsigned long)gx_lv_density());
        }
    }
}
static int options_result(ui_dialog *dialog, unsigned action, void *unused) {
    unsigned old_header = prefs.header, old_delimiter = prefs.delimiter;
    (void)dialog;
    (void)unused;
    if (!action)
        return 1;
    prefs.header = lv_obj_has_state(option_header, LV_STATE_CHECKED);
    prefs.delimiter = delimiters[ui_radio_selected(option_delimiter)];
    if (current_path[0] && !queue_load(current_path)) {
        prefs.header = old_header;
        prefs.delimiter = old_delimiter;
        lv_label_set_text(option_error, paths->error);
        return 0;
    }
    save_warning = settings_save() < 0;
    if (!current_path[0])
        tell(save_warning ? "Settings could not be saved" : "Options saved");
    if (trace)
        fprintf(stderr, "DATA_OPTIONS header=%u delimiter=%u\n", prefs.header, prefs.delimiter);
    return 1;
}
static void options(void) {
    ui_dialog *dialog;
    lv_obj_t *body;
    unsigned i;
    if (busy)
        return;
    dialog = ui_dialog_open("Data options", 400, options_result, NULL);
    if (!dialog)
        return;
    body = ui_dialog_body(dialog);
    option_header = ui_checkbox(body, "First row contains column names");
    if (prefs.header)
        lv_obj_add_state(option_header, LV_STATE_CHECKED);
    ui_label(body, "Delimiter", UI_SECONDARY);
    for (i = 0; i < 4; i++)
        if (delimiters[i] == prefs.delimiter)
            break;
    option_choice = i < 4 ? i : 0;
    option_delimiter = ui_radio_list(body, delimiter_names, 4, option_choice);
    option_error = ui_label(body, "", UI_ERROR);
    lv_obj_set_width(option_error, lv_pct(100));
    lv_label_set_long_mode(option_error, LV_LABEL_LONG_MODE_WRAP);
    ui_dialog_button(dialog, "Cancel", 0, 0);
    ui_dialog_button(dialog, "Apply", 1, 1);
    if (trace)
        fprintf(stderr, "DATA_DIALOG options\n");
}
static void copy_selection(int whole_row) {
    char *buffer;
    const char *s;
    unsigned capacity = 32768U;
    int error = 0;
    if (!document.row_count)
        return;
    buffer = malloc(capacity);
    if (!buffer) {
        ui_message("Cannot copy", "Not enough memory.");
        return;
    }
    if (whole_row)
        error = csv_copy_row(&document, ui_table_row(table), buffer, capacity);
    else {
        s = csv_cell(&document, ui_table_row(table), ui_table_column(table));
        if (strlen(s) >= capacity)
            error = -1;
        else
            strcpy(buffer, s);
    }
    if (error)
        ui_message("Cannot copy", "The selection exceeds the 32 KiB clipboard limit.");
    else if (gx_lv_clipboard(buffer, capacity, 1))
        ui_message("Cannot copy", "The clipboard is unavailable.");
    else {
        tell(whole_row ? "Row copied" : "Cell copied");
        if (trace)
            fprintf(stderr, "DATA_COPY %s %lu\n", whole_row ? "row" : "cell",
                    (unsigned long)strlen(buffer));
    }
    free(buffer);
}
static void sort_column(void *unused, unsigned column) {
    int descending = document.sort_column == (int)column ? !document.descending : 0;
    (void)unused;
    if (busy || !document.row_count)
        return;
    sort_source = document.order[ui_table_row(table)];
    selected_column = ui_table_column(table);
    sort_column_number = column;
    sort_descending = descending;
    csv_work = csv_sort_begin(&document, column, descending, paths->error, sizeof(paths->error));
    if (!csv_work) {
        ui_message("Cannot sort", paths->error);
        return;
    }
    if (!work_begin(WORK_SORT))
        ui_message("Cannot sort", paths->error);
}
static void dispatch(unsigned action, void *unused) {
    unsigned i;
    (void)unused;
    if (busy && action != CLOSE)
        return;
    if (action >= RECENT_FIRST && action < RECENT_FIRST + prefs.count) {
        if (!queue_load(prefs.recent[action - RECENT_FIRST]))
            ui_message("Cannot open file", paths->error);
        return;
    }
    switch (action) {
    case OPEN:
        open_file();
        break;
    case RELOAD:
        if (current_path[0] && !queue_load(current_path))
            ui_message("Cannot open file", paths->error);
        break;
    case OPTIONS:
        options();
        break;
    case COPY_CELL:
        copy_selection(0);
        break;
    case COPY_ROW:
        copy_selection(1);
        break;
    case CLOSE:
        gx_lv_finish();
        break;
    case ORIGINAL:
        if (!document.row_count)
            break;
        original_order = malloc((unsigned long)(document.record_count ? document.record_count : 1) *
                                sizeof(*original_order));
        if (!original_order) {
            ui_message("Cannot restore order", "Not enough memory.");
            break;
        }
        original_row = 0;
        sort_source = document.order[ui_table_row(table)];
        selected_column = ui_table_column(table);
        sort_column_number = ui_table_column(table);
        sort_descending = 0;
        if (!work_begin(WORK_ORIGINAL))
            ui_message("Cannot restore order", paths->error);
        break;
    case WIDER:
        ui_table_column_width(table, ui_table_column(table),
                              ui_table_get_width(table, ui_table_column(table)) + 16);
        break;
    case NARROWER: {
        unsigned w = ui_table_get_width(table, ui_table_column(table));
        ui_table_column_width(table, ui_table_column(table), w > 80 ? w - 16 : 64);
        break;
    }
    case RESET_WIDTHS:
        for (i = 0; i < CSV_MAX_COLUMNS; i++)
            ui_table_column_width(table, i, 144);
        break;
    case CLEAR_RECENT:
        prefs.count = 0;
        save_warning = settings_save() < 0;
        tell("Recent files cleared");
        update_actions();
        break;
    case DETAILS:
        ui_split_show_second(workspace, !ui_split_second_visible(workspace));
        inspect_cell();
        ui_table_focus(table);
        break;
    case PLOT:
        ui_split_show_second(plot_split, !ui_split_second_visible(plot_split));
        data_plot_visible(plots, prefs.plot);
        ui_table_focus(table);
        break;
    case EXPORT_PLOT:
        data_plot_export(plots);
        break;
    case ABOUT:
        ui_message("Data Viewer",
                   "Browse CSV and TSV files. Click a column heading to sort; drag its right edge "
                   "to resize. Arrow keys select cells. Ctrl+C copies a cell; Ctrl+Shift+C copies "
                   "a row. Your data files are opened read-only.");
        break;
    }
}
static void menu_dispatch(unsigned action, void *unused) {
    (void)unused;
    if (action >= RECENT_FIRST)
        dispatch(action, NULL);
    else
        ui_actions_invoke(actions, action);
}
static void file_items(lv_event_t *event) {
    struct ui_menu_item items[RECENTS + 5];
    char captions[RECENTS][180];
    unsigned i, n = 0;
    (void)event;
    if (busy)
        return;
    ui_actions_item(actions, OPEN, &items[n++]);
    ui_actions_item(actions, RELOAD, &items[n++]);
    for (i = 0; i < prefs.count; i++) {
        snprintf(captions[i], sizeof(captions[i]), "%s  (%s)", basename_of(prefs.recent[i]),
                 prefs.recent[i]);
        items[n++] = (struct ui_menu_item){captions[i], "", "file", RECENT_FIRST + i, 0};
    }
    ui_actions_item(actions, EXPORT_PLOT, &items[n++]);
    ui_actions_item(actions, CLEAR_RECENT, &items[n++]);
    ui_actions_item(actions, CLOSE, &items[n++]);
    if (ui_menu_open(file_menu, items, n, menu_dispatch, NULL) && trace)
        fprintf(stderr, "DATA_MENU file\n");
}
static void view_items(lv_event_t *event) {
    static const unsigned ids[] = {OPTIONS, ORIGINAL, WIDER, NARROWER, RESET_WIDTHS, DETAILS, PLOT};
    (void)event;
    if (!busy && ui_actions_menu(actions, view_menu, ids, 7) && trace)
        fprintf(stderr, "DATA_MENU view\n");
}
static void help_items(lv_event_t *event) {
    static const unsigned ids[] = {ABOUT};
    (void)event;
    ui_actions_menu(actions, help_menu, ids, 1);
}
static void menu_key(lv_event_t *event) {
    if (lv_event_get_key(event) == LV_KEY_DOWN) {
        lv_obj_t *obj = lv_event_get_target_obj(event);
        if (obj == file_menu)
            file_items(event);
        else if (obj == view_menu)
            view_items(event);
        else
            help_items(event);
    }
}
static void clipboard(unsigned action) {
    if (action == GX_ACTION_COPY)
        ui_actions_invoke(actions, COPY_CELL);
}
static int shortcuts(unsigned key, int down, unsigned modifiers) {
    int ctrl = modifiers & 1;
    if (key == 41 && !ui_overlay_active() && data_plot_running(plots)) {
        if (down)
            data_plot_cancel(plots);
        return 1;
    }
    if (ui_actions_key(actions, key, down, modifiers))
        return 1;
    if (ui_overlay_active())
        return 0;
    if (busy) {
        if (key == 41 && down)
            work_cancelled = 1;
        return key != 43 && key != 40;
    }
    if (key == 67) {
        if (down)
            lv_group_focus_obj(file_menu);
        return 1;
    }
    if (ui_table_focused(table)) {
        if (ctrl && (key == 74 || key == 77)) {
            if (down && document.row_count)
                ui_table_select(table, key == 74 ? 0 : document.row_count - 1,
                                key == 74 ? 0 : document.columns - 1, 1);
            return 1;
        }
        if (key == 75 || key == 78) {
            if (down)
                ui_table_page(table, key == 75 ? -1 : 1);
            return 1;
        }
    }
    return 0;
}
static void resize(unsigned w, unsigned h) {
    static const unsigned widths[] = {88, 104, 104, 112, 112};
    unsigned i;
    screen_width = w;
    screen_height = h;
    for (i = 0; i < 5; i++)
        gx_lv_width(lv_obj_get_child(toolbar, i), widths[i]);
    if (open_dialog && ui_overlay_active())
        ui_path_density(open_dialog);
    gx_lv_height(menubar, UI_CONTROL);
    gx_lv_height(toolbar, UI_CONTROL);
    gx_lv_height(title, 24);
    gx_lv_height(status, 20);
    gx_lv_height(detail, 20);
    ui_split_density(workspace);
    ui_split_density(plot_split);
    ui_progress_density(progress);
    data_plot_density(plots);
    ui_table_density(table);
}
static lv_obj_t *tool(unsigned width, unsigned action) {
    lv_obj_t *button = ui_actions_button(actions, toolbar, action, UI_BUTTON);
    if (button)
        gx_lv_width(button, width);
    return button;
}
static int ui(void) {
    lv_obj_t *screen = lv_screen_active(), *spacer, *panel;
    unsigned i;
    ui_screen();
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    menubar = ui_stack(screen, LV_FLEX_FLOW_ROW, UI_SPACE);
    lv_obj_set_flex_align(menubar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    file_menu = ui_button(menubar, "File", UI_TOOL);
    view_menu = ui_button(menubar, "View", UI_TOOL);
    help_menu = ui_button(menubar, "Help", UI_TOOL);
    if (!ui_actions_button(actions, menubar, PLOT, UI_TOOL))
        return 0;
    lv_obj_add_event_cb(file_menu, file_items, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(view_menu, view_items, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(help_menu, help_items, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(file_menu, menu_key, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(view_menu, menu_key, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(help_menu, menu_key, LV_EVENT_KEY, NULL);
    spacer = ui_stack(menubar, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_width(spacer, 0);
    lv_obj_set_height(spacer, 0);
    lv_obj_set_flex_grow(spacer, 1);
    summary = ui_label(menubar, "CSV / TSV", UI_SECONDARY);
    title = ui_label(screen, "Open a data file", UI_HEADING);
    lv_obj_set_width(title, lv_pct(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    toolbar = ui_stack(screen, LV_FLEX_FLOW_ROW, UI_SPACE);
    if (!tool(88, OPEN) || !tool(104, RELOAD) || !tool(104, OPTIONS) ||
        !tool(112, COPY_CELL) || !tool(112, COPY_ROW))
        return 0;
    progress = ui_progress_create(screen, work_cancel, NULL);
    if (!progress)
        return 0;
    lv_obj_set_hidden(ui_progress_object(progress), true);
    workspace = ui_split_create(screen, UI_SPLIT_HORIZONTAL);
    if (!workspace)
        return 0;
    lv_obj_set_height(ui_split_object(workspace), 0);
    lv_obj_set_flex_grow(ui_split_object(workspace), 1);
    ui_split_minimum(workspace, 280, 180);
    ui_split_position(workspace, prefs.split);
    ui_split_show_second(workspace, prefs.details);
    lv_obj_add_event_cb(ui_split_object(workspace), split_changed, LV_EVENT_VALUE_CHANGED, NULL);
    plot_split = ui_split_create(ui_split_pane(workspace, 0), UI_SPLIT_VERTICAL);
    if (!plot_split)
        return 0;
    lv_obj_set_size(ui_split_object(plot_split), lv_pct(100), lv_pct(100));
    ui_split_minimum(plot_split, 112, 220);
    ui_split_position(plot_split, prefs.plot_split);
    ui_split_show_second(plot_split, prefs.plot);
    lv_obj_add_event_cb(ui_split_object(plot_split), plot_split_changed, LV_EVENT_VALUE_CHANGED, NULL);
    table = ui_table_create(ui_split_pane(plot_split, 0), value, NULL);
    if (!table)
        return 0;
    lv_obj_set_height(ui_table_object(table), lv_pct(100));
    plots = data_plot_create(ui_split_pane(plot_split, 1), &document, table,
                             prefs.plot_x, prefs.plot_y, prefs.plot_mode, plot_changed, NULL);
    if (!plots)
        return 0;
    data_plot_trace(plots, trace);
    panel = ui_panel(ui_split_pane(workspace, 1));
    lv_obj_set_size(panel, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    ui_label(panel, "Selected cell", UI_HEADING);
    inspector_meta = ui_label(panel, "No cell selected", UI_SECONDARY);
    inspector_value = ui_label(panel, "Open a data file to inspect a cell.", UI_BODY);
    lv_obj_set_width(inspector_value, lv_pct(100));
    inspector_hint = ui_label(panel, "", UI_SECONDARY);
    lv_obj_set_width(inspector_hint, lv_pct(100));
    ui_table_callbacks(table, selection, sort_column);
    for (i = 0; i < CSV_MAX_COLUMNS; i++)
        ui_table_column_width(table, i, prefs.widths[i]);
    status = ui_label(screen, "Open a CSV or TSV file to begin.", UI_SECONDARY);
    lv_obj_set_width(status, lv_pct(100));
    lv_label_set_long_mode(status, LV_LABEL_LONG_MODE_DOTS);
    detail = ui_label(screen, "Arrow keys: select  |  Ctrl+C: copy cell  |  Ctrl+Shift+C: copy row",
                      UI_BODY);
    lv_obj_set_width(detail, lv_pct(100));
    lv_label_set_long_mode(detail, LV_LABEL_LONG_MODE_DOTS);
    ui_table_bind(table, 0, 0);
    data_plot_visible(plots, prefs.plot);
    update_actions();
    resize(screen_width, screen_height);
    return 1;
}
int main(int argc, char **argv) {
    int code, i, initial_plot = -1;
    const char *initial = NULL;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--trace"))
            trace = 1;
        else if (!strcmp(argv[i], "--plot"))
            initial_plot = 1;
        else if (!strcmp(argv[i], "--no-plot"))
            initial_plot = 0;
        else if (!strcmp(argv[i], "--help")) {
            puts("Usage: lvdata [--trace] [--plot|--no-plot] [FILE]\nCSV/TSV data viewer. Ctrl+O Open; F5 Reload; F8 "
                 "Options; F10 Menus.\nClick headers to sort; drag header edges to resize. Ctrl+C "
                 "copies a cell; Ctrl+Shift+C copies a row. Ctrl+D Details; Ctrl+P Plot; Ctrl+E Export.");
            return 0;
        } else if (initial) {
            fputs("Supply one data file.\n", stderr);
            return 1;
        } else
            initial = argv[i];
    }
    /* Keep document-sized storage out of C16's bounded global address range. */
    prefs.recent = calloc(RECENTS, sizeof(*prefs.recent));
    paths = calloc(1, sizeof(*paths));
    if (!prefs.recent || !paths) {
        fputs("Not enough memory for recent files.\n", stderr);
        free(prefs.recent);
        free(paths);
        return 1;
    }
    settings_load();
    if (initial_plot >= 0)
        prefs.plot = initial_plot;
    gx_lv_minimum(640, 420);
    if (gx_lv_open("Data Viewer", screen_width, screen_height) < 0) {
        free(prefs.recent);
        free(paths);
        return 1;
    }
    actions = ui_actions_create(commands, sizeof(commands) / sizeof(commands[0]), dispatch, NULL);
    if (!actions) {
        gx_lv_close();
        free(prefs.recent);
        free(paths);
        return 1;
    }
    if (!ui()) {
        fputs("Not enough memory to open Data Viewer.\n", stderr);
        data_plot_delete(plots);
        plots = NULL;
        gx_lv_close();
        ui_actions_delete(actions);
        free(prefs.recent);
        free(paths);
        return 1;
    }
    gx_lv_handlers(NULL, clipboard, resize);
    gx_lv_shortcuts(shortcuts);
    if (initial && !queue_load(initial))
        ui_message("Cannot open file", paths->error[0] ? paths->error : "Invalid file path.");
    if (trace)
        fprintf(stderr, "DATA_READY\n");
    code = gx_lv_run();
    work_stop();
    save_warning = settings_save() < 0;
    data_plot_delete(plots);
    plots = NULL;
    gx_lv_close();
    ui_actions_delete(actions);
    csv_free(&document);
    free(prefs.recent);
    free(paths);
    return code;
}
