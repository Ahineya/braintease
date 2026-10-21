/* SPDX-License-Identifier: MIT */
#ifndef GRAPHITE_UI_PLOT_H
#define GRAPHITE_UI_PLOT_H
#include "ui.h"
#include "plot.h"
typedef struct ui_plot ui_plot;
typedef void (*ui_plot_select_fn)(void *user, uint32_t source_row);
ui_plot *ui_plot_create(lv_obj_t *parent);
lv_obj_t *ui_plot_object(ui_plot *plot);
/* Borrowed immutable model. Unbind before deleting the model. Binding fits it. */
void ui_plot_bind(ui_plot *plot, const plot_model *model);
void ui_plot_labels(ui_plot *plot, const char *xlabel, const char *ylabel);
void ui_plot_mode(ui_plot *plot, unsigned mode);
void ui_plot_message(ui_plot *plot, const char *message);
void ui_plot_fit(ui_plot *plot);
int ui_plot_get_view(ui_plot *plot, struct plot_view *view);
int ui_plot_set_view(ui_plot *plot, const struct plot_view *view);
void ui_plot_pan(ui_plot *plot, double horizontal, double vertical);
void ui_plot_zoom(ui_plot *plot, double factor);
void ui_plot_density(ui_plot *plot);
void ui_plot_refresh(ui_plot *plot);
void ui_plot_focus(ui_plot *plot);
int ui_plot_focused(ui_plot *plot);
void ui_plot_callback(ui_plot *plot, ui_plot_select_fn select, void *user);
/* Select by model index (not source row). PLOT_NO_ROW clears the selection. */
void ui_plot_select(ui_plot *plot, uint32_t point_index);
uint32_t ui_plot_selected(ui_plot *plot);
/* Cached viewport-work counters, useful to assert bounded rendering. */
void ui_plot_stats(ui_plot *plot, struct plot_stats *stats);
#endif
