/* SPDX-License-Identifier: MIT */
#ifndef GRAPHITE_PLOT_H
#define GRAPHITE_PLOT_H
#include <stdint.h>

#define PLOT_NO_ROW UINT32_MAX
#define PLOT_MAX_POINTS 1000000UL
typedef struct plot_model plot_model;
struct plot_point {
    double x, y;
    uint32_t row;
    unsigned valid;
};
struct plot_view { double xmin, xmax, ymin, ymax; };
struct plot_vertex {
    double x, y;
    uint32_t row, index;
    unsigned connect;
};
struct plot_stats {
    uint32_t nodes, points, vertices, aggregates;
};
enum plot_mode { PLOT_LINE, PLOT_SCATTER };
typedef struct plot_export_job plot_export_job;

/* Owns its points, independent of any document. Append then prepare; once
 * prepared the model is immutable. Invalid/nonfinite samples remain gaps and
 * retain their source identity. Point storage and the small block hierarchy
 * are allocated on the heap. Capacity zero is an empty model. */
plot_model *plot_model_create(uint32_t capacity);
void plot_model_delete(plot_model *model);
/* 0 success; -1 capacity exhausted, NULL model, or preparation already started. */
int plot_model_append(plot_model *model, double x, double y, uint32_t row, int valid);
/* One step combines at most budget hierarchy nodes. 1 ready; 0 more work;
 * -1 invalid model. An empty model prepares successfully. */
int plot_model_prepare_step(plot_model *model, uint32_t budget);
int plot_model_ready(const plot_model *model);
uint32_t plot_model_count(const plot_model *model);
uint32_t plot_model_valid(const plot_model *model);
uint32_t plot_model_invalid(const plot_model *model);
const struct plot_point *plot_model_point(const plot_model *model, uint32_t index);
/* Fit expands constant axes, pads nonconstant axes where representable, and
 * accepts finite extremes without overflowing a span. 0 means no valid data. */
int plot_model_fit(const plot_model *model, struct plot_view *view);
int plot_view_valid(const struct plot_view *view);
int plot_view_pan(struct plot_view *view, double horizontal, double vertical);
/* factor < 1 zooms in; anchors are fractions of each data axis (0..1). */
int plot_view_zoom(struct plot_view *view, double factor, double xanchor, double yanchor);
double plot_axis_fraction(double value, double low, double high);
double plot_axis_value(double low, double high, double fraction);
/* Same clipped coordinates feed the on-screen and SVG renderers. */
int plot_clip_segment(const struct plot_view *view, double x0, double y0,
                       double x1, double y1, double *nx0, double *ny0,
                       double *nx1, double *ny1);
/* At most min(capacity, 4*logical_width+64) representatives, with original
 * point indices and row identities. Hierarchical queries are bounded by this
 * budget rather than total document size. Aggregates retain both axis extrema
 * in source order. Mixed-gap aggregates use isolated representatives rather
 * than inventing connections across missing data. */
uint32_t plot_model_geometry(const plot_model *model, const struct plot_view *view,
                             unsigned logical_width, struct plot_vertex *out,
                             uint32_t capacity, struct plot_stats *stats);
/* Writes a standalone SVG via a sibling temporary file and replacement rename.
 * Labels are XML escaped. Failure preserves the existing destination. The
 * caller owns overwrite confirmation. Returns 0 success, -1 error. */
int plot_export_svg(const plot_model *model, const struct plot_view *view, unsigned mode,
                     const char *xlabel, const char *ylabel, const char *path,
                     unsigned width, unsigned height, char *error, unsigned capacity);
/* Incremental export snapshots the bounded viewport geometry at begin, then
 * writes at most budget representatives per step. 1 complete; 0 more work;
 * -1 failed. Delete at any time cancels an unfinished export and removes only
 * its private temporary file. The model is not borrowed after begin returns.
 * Progress is 0..1000; the destination is replaced only on successful finish. */
plot_export_job *plot_export_begin(const plot_model *model, const struct plot_view *view,
                                    unsigned mode, const char *xlabel, const char *ylabel,
                                    const char *path, unsigned width, unsigned height,
                                    char *error, unsigned capacity);
int plot_export_step(plot_export_job *job, uint32_t budget);
unsigned plot_export_progress(const plot_export_job *job);
const char *plot_export_error(const plot_export_job *job);
void plot_export_delete(plot_export_job *job);
#endif
