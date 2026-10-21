/* SPDX-License-Identifier: MIT */
#ifndef GRAPHITE_DATA_PLOT_H
#define GRAPHITE_DATA_PLOT_H
#include "ui-plot.h"
#include "ui-forms.h"
#include "csv.h"
typedef struct data_plot data_plot;
typedef void (*data_plot_changed_fn)(void *);
data_plot *data_plot_create(lv_obj_t *parent, struct csv_document *document,
                             ui_table *table, unsigned x, unsigned y, unsigned mode,
                             data_plot_changed_fn changed, void *user);
lv_obj_t *data_plot_object(data_plot *plot);
/* Called immediately after a document/order commit, before more user input. */
void data_plot_document(data_plot *plot);
void data_plot_visible(data_plot *plot, int visible);
void data_plot_enabled(data_plot *plot, int enabled);
void data_plot_options(data_plot *plot, unsigned *x, unsigned *y, unsigned *mode);
void data_plot_select(data_plot *plot, uint32_t row);
void data_plot_density(data_plot *plot);
void data_plot_trace(data_plot *plot, int enabled);
void data_plot_cancel(data_plot *plot);
int data_plot_running(data_plot *plot);
int data_plot_can_export(data_plot *plot);
void data_plot_export(data_plot *plot);
/* Deletes its root and releases timers, dialogs, models and mappings. */
void data_plot_delete(data_plot *plot);
#endif
