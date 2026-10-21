/* SPDX-License-Identifier: MIT */
/* CSV adapter for the document-independent plot and shared form components. */
#include "data-plot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct data_plot {
    lv_obj_t *root, *status, *fit, *refresh, *out, *plus, *minus;
    ui_plot *view;
    ui_choice *xc, *yc, *mc;
    ui_progress *progress;
    ui_path *save;
    lv_timer_t *timer;
    plot_export_job *export_job;
    csv_number *numeric;
    char *export_path;
    struct csv_document *doc;
    ui_table *table;
    plot_model *model;
    uint32_t *row_map, row;
    unsigned x, y, mode, visible, enabled, binding, dirty, busy, cancelled;
    unsigned progress_value, sample_phase, sample_good, auto_y, auto_row, auto_col, choices_ok, closing, tracing, focus_cancel, trace;
    double sample_x, sample_y;
    char xlabel[96], ylabel[96];
    data_plot_changed_fn changed;
    void *user;
};
static void rebuild(data_plot *p);
static void controls(data_plot *p);
static void trace_geometry(data_plot *p);
static void caption(char *out, unsigned cap, const char *s);
static void heading(data_plot *p, char *out, unsigned column) {
    const char *text = csv_heading(p->doc, column);
    if (text && *text)
        caption(out, 96, text);
    else
        snprintf(out, 96, "Column %u", column + 1);
}

static void caption(char *out, unsigned cap, const char *s) {
    unsigned at = 0;
    while (*s && at + 5 < cap) {
        unsigned c = (unsigned char)*s, n = c < 128 ? 1 : c < 224 ? 2 : c < 240 ? 3 : 4, i;
        if (at + n + 4 >= cap)
            break;
        if (c < 32 || c == 127) {
            out[at++] = ' ';
            s++;
        } else
            for (i = 0; i < n && *s; i++)
                out[at++] = *s++;
    }
    if (*s) {
        out[at++] = '.';
        out[at++] = '.';
        out[at++] = '.';
    }
    out[at] = 0;
}
static void notify(data_plot *p) {
    if (p->closing)
        return;
    controls(p);
    if (p->changed)
        p->changed(p->user);
}
static void state(lv_obj_t *obj, int enabled) {
    if (enabled)
        lv_obj_remove_state(obj, LV_STATE_DISABLED);
    else
        lv_obj_add_state(obj, LV_STATE_DISABLED);
}
int data_plot_can_export(data_plot *p) {
    return p && p->enabled && !p->busy && p->model && plot_model_ready(p->model) && plot_model_valid(p->model);
}
static void controls(data_plot *p) {
    int edit = p->enabled && p->choices_ok && !p->busy && p->doc->columns;
    int ready = p->enabled && data_plot_can_export(p);
    state(ui_choice_object(p->xc), edit);
    state(ui_choice_object(p->yc), edit);
    state(ui_choice_object(p->mc), p->enabled && !p->busy);
    state(p->refresh, edit && p->doc->row_count);
    state(p->fit, ready);
    state(p->plus, ready);
    state(p->minus, ready);
    state(p->out, ready);
    state(ui_plot_object(p->view), p->enabled && !p->busy);
}
static void discard_model(data_plot *p) {
    ui_plot_bind(p->view, NULL);
    plot_model_delete(p->model);
    p->model = NULL;
    free(p->row_map);
    p->row_map = NULL;
}
static void end_job(data_plot *p) {
    lv_obj_t *focus = lv_group_get_focused(gx_lv_group());
    int restore = 0;
    while (focus) {
        if (focus == ui_progress_object(p->progress)) {
            restore = 1;
            break;
        }
        focus = lv_obj_get_parent(focus);
    }
    if (p->timer) {
        lv_timer_delete(p->timer);
        p->timer = NULL;
    }
    p->busy = 0;
    if (!p->closing)
        controls(p);
    ui_progress_running(p->progress, 0);
    lv_obj_set_hidden(ui_progress_object(p->progress), true);
    notify(p);
    if (restore && !p->closing && p->enabled && p->visible) {
        if (p->model && plot_model_ready(p->model))
            ui_plot_focus(p->view);
        else
            ui_table_focus(p->table);
    }
}
void data_plot_cancel(data_plot *p) {
    if (!p || !p->busy)
        return;
    if (p->export_job) {
        plot_export_delete(p->export_job);
        p->export_job = NULL;
        free(p->export_path);
        p->export_path = NULL;
        if (p->trace)
            fprintf(stderr, "DATA_PLOT_EXPORT_CANCEL\n");
    } else {
        /* Building never mutates the CSV or its order. */
        discard_model(p);
        p->dirty = 1;
        if (p->trace)
            fprintf(stderr, "DATA_PLOT_CANCEL\n");
    }
    end_job(p);
    if (!p->model)
        ui_plot_message(p->view, "Plot preparation cancelled.");
    lv_label_set_text(p->status, "Cancelled");
}
int data_plot_running(data_plot *p) { return p && p->busy; }
void data_plot_trace(data_plot *p, int enabled) { p->trace = !!enabled; }
static void cancel_clicked(ui_progress *progress, void *user) {
    data_plot *p = user;
    (void)progress;
    p->cancelled = 1;
}
static void prepared(data_plot *p) {
    char text[160];
    uint32_t valid = plot_model_valid(p->model), skipped = plot_model_invalid(p->model);
    p->dirty = 0;
    ui_plot_bind(p->view, p->model);
    ui_plot_mode(p->view, p->mode);
    ui_plot_labels(p->view, p->xlabel, p->ylabel);
    if (skipped)
        snprintf(text, sizeof(text), "%lu plotted  |  %lu missing or non-numeric",
                 (unsigned long)valid, (unsigned long)skipped);
    else
        snprintf(text, sizeof(text), "%lu samples  |  Drag: pan  |  +/-: zoom  |  Home: fit", (unsigned long)valid);
    lv_label_set_text(p->status, text);
    if (!valid)
        ui_plot_message(p->view, "No finite numeric samples. Choose another column.");
    end_job(p);
    data_plot_select(p, ui_table_row(p->table));
    if (p->trace)
        fprintf(stderr, "DATA_PLOT_READY points=%lu valid=%lu invalid=%lu x=%u y=%u mode=%u\n",
                (unsigned long)plot_model_count(p->model), (unsigned long)valid,
                (unsigned long)skipped, p->x, p->y, p->mode);
    trace_geometry(p);
}
static void tick(lv_timer_t *timer) {
    data_plot *p = lv_timer_get_user_data(timer);
    uint32_t start = lv_tick_get();
    unsigned rounds = 0;
    int done = 0;
    if (p->focus_cancel && !ui_overlay_active()) {
        ui_progress_focus_cancel(p->progress);
        p->focus_cancel = 0;
    }
    if (p->cancelled) {
        data_plot_cancel(p);
        return;
    }
    if (p->export_job) {
        int result = plot_export_step(p->export_job, 64);
        ui_progress_update(p->progress, "Exporting plot...", plot_export_progress(p->export_job), 1000);
        if (result) {
            char error[200];
            snprintf(error, sizeof(error), "%s", result < 0 ? plot_export_error(p->export_job) : "");
            if (p->trace)
                fprintf(stderr, result > 0 ? "DATA_PLOT_EXPORT %s\n" : "DATA_PLOT_EXPORT_ERROR %s\n",
                        result > 0 ? p->export_path : error);
            plot_export_delete(p->export_job);
            p->export_job = NULL;
            free(p->export_path);
            p->export_path = NULL;
            end_job(p);
            lv_label_set_text(p->status, result > 0 ? "Plot exported" : error);
        }
        return;
    }
    do {
        if (p->auto_y) {
            unsigned count = 0, limit = p->doc->row_count < 16 ? p->doc->row_count : 16;
            while (p->auto_y && count++ < 16) {
                int result;
                if (!p->sample_phase) {
                    csv_number_reset(p->numeric, csv_cell(p->doc, p->auto_row, p->auto_col));
                    p->sample_phase = 1;
                }
                result = csv_number_step(p->numeric, 256);
                if (!result)
                    continue;
                p->sample_phase = 0;
                if (result > 0 || (++p->auto_row >= limit && ++p->auto_col >= p->doc->columns)) {
                    p->y = result > 0 ? p->auto_col : 0;
                    p->auto_y = 0;
                    p->binding = 1;
                    ui_choice_select(p->yc, p->y);
                    p->binding = 0;
                    heading(p, p->ylabel, p->y);
                    ui_plot_labels(p->view, p->xlabel, p->ylabel);
                } else if (p->auto_row >= limit)
                    p->auto_row = 0;
            }
        } else if (p->row < p->doc->row_count) {
            unsigned count = 0;
            while (p->row < p->doc->row_count && count++ < 64) {
                int result;
                uint32_t source = p->doc->order[p->row];
                if (!p->sample_phase) {
                    p->sample_x = (double)p->row + 1.0;
                    p->sample_y = 0.0;
                    csv_number_reset(p->numeric, csv_cell(p->doc, p->row, p->y));
                    p->sample_phase = 1;
                }
                result = csv_number_step(p->numeric, 256);
                if (!result)
                    continue;
                if (p->sample_phase == 1) {
                    p->sample_good = result > 0;
                    if (result > 0)
                        p->sample_y = csv_number_value(p->numeric);
                    if (p->x) {
                        csv_number_reset(p->numeric, csv_cell(p->doc, p->row, p->x - 1));
                        p->sample_phase = 2;
                        continue;
                    }
                } else {
                    p->sample_good = p->sample_good && result > 0;
                    if (result > 0)
                        p->sample_x = csv_number_value(p->numeric);
                }
                p->row_map[source] = p->row;
                if (plot_model_append(p->model, p->sample_x, p->sample_y, source, p->sample_good) < 0) {
                    discard_model(p);
                    end_job(p);
                    ui_plot_message(p->view, "Could not prepare this plot.");
                    return;
                }
                p->row++;
                p->sample_phase = 0;
            }
        } else {
            int result = plot_model_prepare_step(p->model, 256);
            if (result < 0) {
                discard_model(p);
                end_job(p);
                ui_plot_message(p->view, "Could not prepare this plot.");
                return;
            }
            done = result == 1;
        }
    } while (!done && ++rounds < 16 && lv_tick_elaps(start) < 4);
    if (done) {
        prepared(p);
        return;
    }
    {
        unsigned fraction = p->doc->row_count
                                ? (unsigned)(p->row * 900UL / p->doc->row_count) : 900;
        if (fraction != p->progress_value) {
            p->progress_value = fraction;
            ui_progress_update(p->progress, p->row < p->doc->row_count ? "Reading samples..."
                                                                     : "Preparing plot...",
                               fraction, 1000);
        }
    }
}
static void rebuild(data_plot *p) {
    if (!p->visible || !p->enabled || !p->choices_ok || !p->doc->row_count || !p->doc->columns)
        return;
    data_plot_cancel(p);
    discard_model(p);
    p->model = plot_model_create(p->doc->row_count);
    p->row_map = malloc((unsigned long)p->doc->record_count * sizeof(*p->row_map));
    if (!p->model || !p->row_map) {
        discard_model(p);
        ui_plot_message(p->view, "Not enough memory to prepare this plot.");
        notify(p);
        return;
    }
    p->row = 0;
    p->sample_phase = p->auto_row = p->auto_col = 0;
    p->cancelled = 0;
    p->busy = 1;
    p->focus_cancel = 1;
    p->progress_value = 1001;
    if (p->x)
        heading(p, p->xlabel, p->x - 1);
    else
        strcpy(p->xlabel, "Row number");
    heading(p, p->ylabel, p->y);
    ui_plot_labels(p->view, p->xlabel, p->ylabel);
    ui_plot_message(p->view, "Preparing plot...");
    ui_progress_running(p->progress, 1);
    lv_obj_set_hidden(ui_progress_object(p->progress), false);
    ui_progress_update(p->progress, "Reading samples...", 0, 1000);
    p->timer = lv_timer_create(tick, 5, p);
    if (!p->timer) {
        data_plot_cancel(p);
        return;
    }
    notify(p);
    trace_geometry(p);
}
static void options(lv_event_t *event) {
    data_plot *p = lv_event_get_user_data(event);
    unsigned x, y, mode;
    if (p->binding || !p->enabled || p->busy)
        return;
    x = ui_choice_selected(p->xc);
    y = ui_choice_selected(p->yc);
    mode = ui_choice_selected(p->mc);
    if (mode != p->mode && x == p->x && y == p->y) {
        p->mode = mode;
        ui_plot_mode(p->view, mode);
    } else {
        p->auto_y = 0;
        p->x = x;
        p->y = y;
        p->mode = mode;
        p->dirty = 1;
        rebuild(p);
    }
    if (p->trace)
        fprintf(stderr, "DATA_PLOT_OPTIONS x=%u y=%u mode=%u\n", p->x, p->y, p->mode);
    notify(p);
}
static void button(lv_event_t *event) {
    data_plot *p = lv_event_get_user_data(event);
    lv_obj_t *obj = lv_event_get_target_obj(event);
    if (!p->enabled || p->busy)
        return;
    if (obj == p->fit)
        ui_plot_fit(p->view);
    else if (obj == p->plus)
        ui_plot_zoom(p->view, 0.8);
    else if (obj == p->minus)
        ui_plot_zoom(p->view, 1.25);
    else if (obj == p->out)
        data_plot_export(p);
    else
        rebuild(p);
}
static void selected(void *user, uint32_t source) {
    data_plot *p = user;
    if (!p->enabled || p->busy || !p->row_map || source >= p->doc->record_count)
        return;
    ui_table_select(p->table, p->row_map[source], p->y, 1);
    if (p->trace)
        fprintf(stderr, "DATA_PLOT_SELECT source=%lu row=%lu\n", (unsigned long)source,
                (unsigned long)p->row_map[source]);
}
void data_plot_select(data_plot *p, uint32_t row) {
    if (p && p->model && !p->busy)
        ui_plot_select(p->view, row < plot_model_count(p->model) ? row : PLOT_NO_ROW);
}
static int save_plot(ui_path *dialog, const char *path, void *user) {
    data_plot *p = user;
    char error[200];
    struct plot_view view;
    size_t length;
    if (!path) {
        p->save = NULL;
        if (p->trace)
            fprintf(stderr, "DATA_PLOT_EXPORT_DIALOG_CANCEL\n");
        return 1;
    }
    length = strlen(path);
    if (length < 4 || strcmp(path + length - 4, ".svg")) {
        ui_path_error(dialog, "Use a filename ending in .svg.");
        return 0;
    }
    if (!data_plot_can_export(p) || !ui_plot_get_view(p->view, &view)) {
        ui_path_error(dialog, "Prepare a plot before exporting.");
        return 0;
    }
    p->export_path = malloc(length + 1);
    if (!p->export_path) {
        ui_path_error(dialog, "Not enough memory to export this plot.");
        return 0;
    }
    strcpy(p->export_path, path);
    p->export_job = plot_export_begin(p->model, &view, p->mode, p->xlabel, p->ylabel, path,
                                      960, 600, error, sizeof(error));
    if (!p->export_job) {
        free(p->export_path);
        p->export_path = NULL;
        ui_path_error(dialog, error);
        return 0;
    }
    p->cancelled = 0;
    p->busy = 1;
    p->focus_cancel = 1;
    ui_progress_running(p->progress, 1);
    lv_obj_set_hidden(ui_progress_object(p->progress), false);
    ui_progress_update(p->progress, "Exporting plot...", 0, 1000);
    p->timer = lv_timer_create(tick, 5, p);
    if (!p->timer) {
        data_plot_cancel(p);
        ui_path_error(dialog, "Could not start plot export.");
        return 0;
    }
    if (p->trace)
        fprintf(stderr, "DATA_PLOT_EXPORT_BEGIN %s\n", path);
    p->save = NULL;
    notify(p);
    return 1;
}
void data_plot_export(data_plot *p) {
    const char *home = getenv("HOME");
    char path[1024];
    if (!p || !p->enabled || !data_plot_can_export(p) || ui_overlay_active())
        return;
    if (!home || !*home)
        home = "/tmp";
    if (strlen(home) + 10 >= sizeof(path))
        strcpy(path, "plot.svg");
    else
        snprintf(path, sizeof(path), "%s/plot.svg", home);
    p->save = ui_path_open("Export plot", UI_PATH_SAVE, path, save_plot, p);
    if (p->save && p->trace)
        fprintf(stderr, "DATA_PLOT_EXPORT_OPEN\n");
}
void data_plot_document(data_plot *p) {
    char **labels;
    unsigned i, count = p->doc->columns;
    data_plot_cancel(p);
    discard_model(p);
    p->dirty = 1;
    if (p->x > count)
        p->x = 0;
    p->auto_y = p->y >= count && count;
    if (p->y >= count)
        p->y = 0;
    p->choices_ok = 0;
    labels = calloc(count + 1, sizeof(*labels));
    if (!labels) {
        ui_plot_message(p->view, "Not enough memory to show column choices.");
        notify(p);
        return;
    }
    for (i = 0; i <= count; i++) {
        labels[i] = malloc(96);
        if (!labels[i])
            break;
        if (i)
            heading(p, labels[i], i - 1);
        else
            strcpy(labels[i], "Row number");
    }
    if (i == count + 1) {
        static const char *empty[] = {"No columns"};
        p->binding = 1;
        p->choices_ok = ui_choice_bind(p->xc, (const char *const *)labels, count + 1, p->x) &&
                        ui_choice_bind(p->yc, count ? (const char *const *)(labels + 1) : empty,
                                       count ? count : 1, p->y);
        p->binding = 0;
    }
    for (i = 0; i <= count; i++)
        free(labels[i]);
    free(labels);
    if (!p->choices_ok) {
        ui_plot_message(p->view, "Not enough memory to show column choices.");
        notify(p);
        return;
    }
    lv_label_set_text(p->status, count ? "Choose columns to plot." : "Open a CSV or TSV file to begin.");
    ui_plot_message(p->view, count ? "Choose columns to plot." : "Open a data file to create a plot.");
    notify(p);
    if (p->visible && !p->busy)
        rebuild(p);
}
void data_plot_visible(data_plot *p, int visible) {
    p->visible = !!visible;
    if (!visible)
        data_plot_cancel(p);
    else if (p->dirty)
        rebuild(p);
}
void data_plot_enabled(data_plot *p, int enabled) {
    if (p->enabled == (unsigned)!!enabled)
        return;
    p->enabled = !!enabled;
    if (!enabled)
        data_plot_cancel(p);
    controls(p);
    if (enabled && p->visible && p->dirty)
        rebuild(p);
}
void data_plot_options(data_plot *p, unsigned *x, unsigned *y, unsigned *mode) {
    *x = p->x;
    *y = p->y;
    *mode = p->mode;
}
void data_plot_density(data_plot *p) {
    gx_lv_width(ui_choice_object(p->mc), 104);
    gx_lv_width(p->minus, 32);
    gx_lv_width(p->plus, 32);
    ui_choice_density(p->xc);
    ui_choice_density(p->yc);
    ui_choice_density(p->mc);
    ui_progress_density(p->progress);
    if (p->save)
        ui_path_density(p->save);
    ui_plot_density(p->view);
    trace_geometry(p);
}
static void deleted(lv_event_t *event) {
    data_plot *p = lv_event_get_user_data(event);
    p->closing = 1;
    p->changed = NULL;
    data_plot_cancel(p);
    if (p->save)
        ui_path_cancel(p->save);
    discard_model(p);
    csv_number_delete(p->numeric);
    free(p);
}
static void trace_rect(const char *name, lv_obj_t *obj) {
    lv_area_t area;
    unsigned density = gx_lv_density();
    if (!obj || lv_obj_is_hidden(obj))
        return;
    lv_obj_get_coords(obj, &area);
    fprintf(stderr, "DATA_PLOT_RECT %s %ld %ld %ld %ld\n", name,
            (long)(area.x1 / (int32_t)density), (long)(area.y1 / (int32_t)density),
            (long)(area.x2 / (int32_t)density), (long)(area.y2 / (int32_t)density));
}
static void trace_geometry(data_plot *p) {
    if (!p->trace || p->closing || p->tracing || !p->visible)
        return;
    p->tracing = 1;
    lv_obj_update_layout(p->root);
    trace_rect("plot", ui_plot_object(p->view));
    trace_rect("x", ui_choice_object(p->xc));
    trace_rect("y", ui_choice_object(p->yc));
    trace_rect("mode", ui_choice_object(p->mc));
    trace_rect("fit", p->fit);
    trace_rect("plus", p->plus);
    trace_rect("minus", p->minus);
    trace_rect("export", p->out);
    trace_rect("update", p->refresh);
    trace_rect("progress", ui_progress_object(p->progress));
    p->tracing = 0;
}
static void resized(lv_event_t *event) {
    trace_geometry(lv_event_get_user_data(event));
}
lv_obj_t *data_plot_object(data_plot *p) { return p->root; }
void data_plot_delete(data_plot *p) {
    if (p)
        lv_obj_delete(p->root);
}
static lv_obj_t *make_button(data_plot *p, lv_obj_t *row, const char *text) {
    lv_obj_t *b = ui_button(row, text, UI_BUTTON);
    lv_obj_add_event_cb(b, button, LV_EVENT_CLICKED, p);
    return b;
}
data_plot *data_plot_create(lv_obj_t *parent, struct csv_document *doc, ui_table *table,
                             unsigned x, unsigned y, unsigned mode,
                             data_plot_changed_fn changed, void *user) {
    static const char *initial[] = {"Row number"}, *empty[] = {"No columns"};
    static const char *modes[] = {"Line", "Scatter"};
    data_plot *p = calloc(1, sizeof(*p));
    lv_obj_t *row;
    if (!p)
        return NULL;
    p->doc = doc;
    p->table = table;
    p->x = x;
    p->y = y;
    p->mode = mode == PLOT_SCATTER ? PLOT_SCATTER : PLOT_LINE;
    p->enabled = 1;
    p->dirty = 1;
    p->changed = changed;
    p->user = user;
    p->root = ui_stack(parent, LV_FLEX_FLOW_COLUMN, UI_SPACE);
    p->numeric = csv_number_create();
    if (!p->numeric)
        goto failed;
    lv_obj_set_size(p->root, lv_pct(100), lv_pct(100));
    row = ui_stack(p->root, LV_FLEX_FLOW_ROW, UI_SPACE);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui_label(row, "X", UI_SECONDARY);
    p->xc = ui_choice_create(row, initial, 1, 0);
    if (!p->xc)
        goto failed;
    lv_obj_set_width(ui_choice_object(p->xc), 0);
    lv_obj_set_flex_grow(ui_choice_object(p->xc), 1);
    ui_label(row, "Y", UI_SECONDARY);
    p->yc = ui_choice_create(row, empty, 1, 0);
    if (!p->yc)
        goto failed;
    lv_obj_set_width(ui_choice_object(p->yc), 0);
    lv_obj_set_flex_grow(ui_choice_object(p->yc), 1);
    row = ui_stack(p->root, LV_FLEX_FLOW_ROW_WRAP, UI_SPACE);
    p->mc = ui_choice_create(row, modes, 2, p->mode);
    if (!p->mc)
        goto failed;
    gx_lv_width(ui_choice_object(p->mc), 104);
    p->refresh = make_button(p, row, "Update");
    p->fit = make_button(p, row, "Fit");
    p->minus = make_button(p, row, "-");
    p->plus = make_button(p, row, "+");
    gx_lv_width(p->minus, 32);
    gx_lv_width(p->plus, 32);
    lv_obj_set_style_min_width(p->minus, 0, 0);
    lv_obj_set_style_min_width(p->plus, 0, 0);
    p->out = make_button(p, row, "Export");
    p->view = ui_plot_create(p->root);
    if (!p->view)
        goto failed;
    lv_obj_set_height(ui_plot_object(p->view), 0);
    lv_obj_set_flex_grow(ui_plot_object(p->view), 1);
    ui_plot_mode(p->view, p->mode);
    ui_plot_callback(p->view, selected, p);
    p->status = ui_label(p->root, "Open a data file to create a plot.", UI_SECONDARY);
    lv_obj_set_width(p->status, lv_pct(100));
    lv_label_set_long_mode(p->status, LV_LABEL_LONG_MODE_DOTS);
    p->progress = ui_progress_create(p->root, cancel_clicked, p);
    if (!p->progress)
        goto failed;
    lv_obj_set_hidden(ui_progress_object(p->progress), true);
    lv_obj_add_event_cb(ui_choice_object(p->xc), options, LV_EVENT_VALUE_CHANGED, p);
    lv_obj_add_event_cb(ui_choice_object(p->yc), options, LV_EVENT_VALUE_CHANGED, p);
    lv_obj_add_event_cb(ui_choice_object(p->mc), options, LV_EVENT_VALUE_CHANGED, p);
    lv_obj_add_event_cb(p->root, deleted, LV_EVENT_DELETE, p);
    lv_obj_add_event_cb(p->root, resized, LV_EVENT_SIZE_CHANGED, p);
    controls(p);
    return p;
failed:
    csv_number_delete(p->numeric);
    lv_obj_delete(p->root);
    free(p);
    return NULL;
}
