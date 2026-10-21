/* SPDX-License-Identifier: MIT */
#include "plot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define BLOCK 32UL
struct summary {
    uint32_t first, last, xmin, xmax, ymin, ymax, valid;
};
struct plot_model {
    struct plot_point *points;
    struct summary *tree;
    uint32_t count, capacity, valid, base, pending;
    unsigned started, ready;
};
static int finite_number(double value) {
    return value == value && value >= -DBL_MAX && value <= DBL_MAX;
}
static double magnitude(double value) { return value < 0 ? -value : value; }
static void add_point(plot_model *m, struct summary *s, uint32_t i) {
    const struct plot_point *p = &m->points[i];
    if (!p->valid)
        return;
    if (!s->valid) {
        s->first = s->last = s->xmin = s->xmax = s->ymin = s->ymax = i;
    } else {
        s->last = i;
        if (p->x < m->points[s->xmin].x)
            s->xmin = i;
        if (p->x > m->points[s->xmax].x)
            s->xmax = i;
        if (p->y < m->points[s->ymin].y)
            s->ymin = i;
        if (p->y > m->points[s->ymax].y)
            s->ymax = i;
    }
    s->valid++;
}
static void combine(plot_model *m, uint32_t i) {
    struct summary *s = &m->tree[i], *a = &m->tree[i * 2], *b = &m->tree[i * 2 + 1];
    if (!a->valid) {
        *s = *b;
        return;
    }
    *s = *a;
    if (!b->valid)
        return;
    s->last = b->last;
    s->valid += b->valid;
    if (m->points[b->xmin].x < m->points[s->xmin].x)
        s->xmin = b->xmin;
    if (m->points[b->xmax].x > m->points[s->xmax].x)
        s->xmax = b->xmax;
    if (m->points[b->ymin].y < m->points[s->ymin].y)
        s->ymin = b->ymin;
    if (m->points[b->ymax].y > m->points[s->ymax].y)
        s->ymax = b->ymax;
}
plot_model *plot_model_create(uint32_t capacity) {
    plot_model *m;
    uint32_t blocks;
    if (capacity > PLOT_MAX_POINTS)
        return NULL;
    m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->capacity = capacity;
    m->base = 1;
    blocks = (capacity + BLOCK - 1) / BLOCK;
    while (m->base < blocks)
        m->base *= 2;
    m->tree = calloc(m->base * 2, sizeof(*m->tree));
    if (capacity)
        m->points = malloc(capacity * sizeof(*m->points));
    if (!m->tree || (capacity && !m->points)) {
        plot_model_delete(m);
        return NULL;
    }
    return m;
}
void plot_model_delete(plot_model *m) {
    if (m) {
        free(m->points);
        free(m->tree);
        free(m);
    }
}
int plot_model_append(plot_model *m, double x, double y, uint32_t row, int valid) {
    struct plot_point *p;
    uint32_t i;
    if (!m || m->started || m->count >= m->capacity)
        return -1;
    i = m->count++;
    p = &m->points[i];
    p->x = x;
    p->y = y;
    p->row = row;
    p->valid = valid && finite_number(x) && finite_number(y);
    if (p->valid) {
        m->valid++;
        add_point(m, &m->tree[m->base + i / BLOCK], i);
    }
    return 0;
}
int plot_model_prepare_step(plot_model *m, uint32_t budget) {
    if (!m)
        return -1;
    if (m->ready)
        return 1;
    if (!m->started) {
        m->started = 1;
        m->pending = m->base - 1;
    }
    while (budget && m->pending) {
        combine(m, m->pending--);
        budget--;
    }
    m->ready = !m->pending;
    return m->ready;
}
int plot_model_ready(const plot_model *m) { return m && m->ready; }
uint32_t plot_model_count(const plot_model *m) { return m ? m->count : 0; }
uint32_t plot_model_valid(const plot_model *m) { return m ? m->valid : 0; }
uint32_t plot_model_invalid(const plot_model *m) { return m ? m->count - m->valid : 0; }
const struct plot_point *plot_model_point(const plot_model *m, uint32_t index) {
    return m && index < m->count ? &m->points[index] : NULL;
}
int plot_view_valid(const struct plot_view *v) {
    return v && finite_number(v->xmin) && finite_number(v->xmax) &&
           finite_number(v->ymin) && finite_number(v->ymax) &&
           v->xmin < v->xmax && v->ymin < v->ymax;
}
double plot_axis_value(double low, double high, double t) {
    double delta = high - low;
    /* Avoid overflowing an opposite-sign span; avoid losing small values in a
     * same-sign weighted sum. Extrapolation is accepted only when finite. */
    return finite_number(delta) ? low + delta * t : low * (1 - t) + high * t;
}
double plot_axis_fraction(double value, double low, double high) {
    double scale, a, b, span;
    if (value <= low)
        return 0.0;
    if (value >= high)
        return 1.0;
    /* Subtract before normalizing when the span is representable. Dividing
     * two large nearby bounds by their magnitude first discards their low
     * bits, visibly moving tight, large-offset views and SVG edge crossings.
     * Opposite extreme bounds can still overflow this span, so retain the
     * scaled form for that domain. */
    span = high - low;
    if (span > 0.0 && finite_number(span))
        return (value - low) / span;
    scale = magnitude(low);
    if (magnitude(high) > scale)
        scale = magnitude(high);
    if (!scale)
        return 0.0;
    a = low / scale;
    b = high / scale;
    return (value / scale - a) / (b - a);
}
static void fit_axis(double low, double high, double *a, double *b) {
    double delta, left, right;
    if (low == high) {
        delta = magnitude(low) * 0.05;
        if (!delta)
            delta = 1;
        left = low - delta;
        right = high + delta;
        if (!finite_number(left))
            left = low;
        if (!finite_number(right))
            right = high;
        if (left == right) {
            left = low == 0 ? -1 : low * 0.95;
            right = low == 0 ? 1 : low;
            if (left > right) {
                delta = left;
                left = right;
                right = delta;
            }
        }
    } else {
        left = plot_axis_value(low, high, -0.035);
        right = plot_axis_value(low, high, 1.035);
        if (!finite_number(left))
            left = low;
        if (!finite_number(right))
            right = high;
    }
    *a = left;
    *b = right;
}
int plot_model_fit(const plot_model *m, struct plot_view *v) {
    const struct summary *s;
    if (!m || !m->ready || !m->valid || !v)
        return 0;
    s = &m->tree[1];
    fit_axis(m->points[s->xmin].x, m->points[s->xmax].x, &v->xmin, &v->xmax);
    fit_axis(m->points[s->ymin].y, m->points[s->ymax].y, &v->ymin, &v->ymax);
    return plot_view_valid(v);
}
int plot_view_pan(struct plot_view *v, double horizontal, double vertical) {
    struct plot_view n;
    if (!plot_view_valid(v) || !finite_number(horizontal) || !finite_number(vertical))
        return 0;
    n.xmin = plot_axis_value(v->xmin, v->xmax, horizontal);
    n.xmax = plot_axis_value(v->xmin, v->xmax, 1 + horizontal);
    n.ymin = plot_axis_value(v->ymin, v->ymax, vertical);
    n.ymax = plot_axis_value(v->ymin, v->ymax, 1 + vertical);
    if (!plot_view_valid(&n))
        return 0;
    *v = n;
    return 1;
}
int plot_view_zoom(struct plot_view *v, double factor, double xanchor, double yanchor) {
    struct plot_view n;
    if (!plot_view_valid(v) || !finite_number(factor) || factor <= 0 ||
        !finite_number(xanchor) || !finite_number(yanchor) ||
        xanchor < 0 || xanchor > 1 || yanchor < 0 || yanchor > 1)
        return 0;
    n.xmin = plot_axis_value(v->xmin, v->xmax, xanchor * (1 - factor));
    n.xmax = plot_axis_value(v->xmin, v->xmax, xanchor + (1 - xanchor) * factor);
    n.ymin = plot_axis_value(v->ymin, v->ymax, yanchor * (1 - factor));
    n.ymax = plot_axis_value(v->ymin, v->ymax, yanchor + (1 - yanchor) * factor);
    if (!plot_view_valid(&n))
        return 0;
    *v = n;
    return 1;
}
static unsigned outcode(const struct plot_view *v, double x, double y) {
    return (x < v->xmin ? 1 : x > v->xmax ? 2 : 0) |
           (y < v->ymin ? 4 : y > v->ymax ? 8 : 0);
}
static double intersect_fraction(double boundary, double a, double b) {
    double numerator = boundary - a, denominator = b - a;
    if (!finite_number(numerator) || !finite_number(denominator)) {
        numerator = boundary * 0.5 - a * 0.5;
        denominator = b * 0.5 - a * 0.5;
    }
    return denominator ? numerator / denominator : 0;
}
static double line_coordinate(double boundary, double a0, double a1, double b0, double b1) {
    double da = a1 - a0, db = b1 - b0, slope, result;
    /* The centered form keeps a line through +/-DBL_MAX usable inside a small
     * viewport: an interpolation parameter would round both edge crossings to
     * exactly 0.5 and erase their distinction. */
    if (!finite_number(da) || !finite_number(db)) {
        da = a1 * 0.5 - a0 * 0.5;
        db = b1 * 0.5 - b0 * 0.5;
    }
    slope = da ? db / da : 0;
    result = plot_axis_value(b0, b1, 0.5) +
             slope * (boundary - plot_axis_value(a0, a1, 0.5));
    if (finite_number(result))
        return result;
    return plot_axis_value(b0, b1, intersect_fraction(boundary, a0, a1));
}
int plot_clip_segment(const struct plot_view *v, double x0, double y0,
                       double x1, double y1, double *nx0, double *ny0,
                       double *nx1, double *ny1) {
    unsigned a, b, code, limit;
    double ox0 = x0, oy0 = y0, ox1 = x1, oy1 = y1;
    if (!plot_view_valid(v) || !finite_number(x0) || !finite_number(y0) ||
        !finite_number(x1) || !finite_number(y1))
        return 0;
    for (limit = 0; limit < 12; limit++) {
        double x, y;
        a = outcode(v, x0, y0);
        b = outcode(v, x1, y1);
        if (!(a | b)) {
            *nx0 = plot_axis_fraction(x0, v->xmin, v->xmax);
            *ny0 = plot_axis_fraction(y0, v->ymin, v->ymax);
            *nx1 = plot_axis_fraction(x1, v->xmin, v->xmax);
            *ny1 = plot_axis_fraction(y1, v->ymin, v->ymax);
            return 1;
        }
        if (a & b)
            return 0;
        code = a ? a : b;
        if (code & 8) {
            y = v->ymax;
            x = line_coordinate(y, oy0, oy1, ox0, ox1);
        } else if (code & 4) {
            y = v->ymin;
            x = line_coordinate(y, oy0, oy1, ox0, ox1);
        } else if (code & 2) {
            x = v->xmax;
            y = line_coordinate(x, ox0, ox1, oy0, oy1);
        } else {
            x = v->xmin;
            y = line_coordinate(x, ox0, ox1, oy0, oy1);
        }
        if (!finite_number(x) || !finite_number(y))
            return 0;
        if (a) {
            x0 = x;
            y0 = y;
        } else {
            x1 = x;
            y1 = y;
        }
    }
    return 0;
}
struct geometry {
    const plot_model *model;
    const struct plot_view *view;
    struct plot_vertex *out;
    struct plot_stats stats;
    uint32_t count, capacity;
    unsigned width, have;
};
static void emit(struct geometry *g, uint32_t index, unsigned connect) {
    const struct plot_point *p = &g->model->points[index];
    struct plot_vertex *out;
    if (g->count >= g->capacity)
        return;
    out = &g->out[g->count++];
    out->x = p->x;
    out->y = p->y;
    out->row = p->row;
    out->index = index;
    out->connect = connect && g->have;
    g->have = 1;
}
static void aggregate(struct geometry *g, const struct summary *s,
                       uint32_t start, uint32_t end) {
    uint32_t ids[6], swap;
    unsigned i, j, full = s->valid == end - start;
    ids[0] = s->first;
    ids[1] = s->last;
    ids[2] = s->xmin;
    ids[3] = s->xmax;
    ids[4] = s->ymin;
    ids[5] = s->ymax;
    for (i = 1; i < 6; i++)
        for (j = i; j && ids[j] < ids[j - 1]; j--) {
            swap = ids[j];
            ids[j] = ids[j - 1];
            ids[j - 1] = swap;
        }
    if (s->first != start)
        g->have = 0;
    for (i = 0; i < 6; i++) {
        if (i && ids[i] == ids[i - 1])
            continue;
        emit(g, ids[i], full || (!i && s->first == start));
    }
    if (s->last + 1 != end)
        g->have = 0;
    g->stats.aggregates++;
}
static void visit(struct geometry *g, uint32_t node, uint32_t start,
                   uint32_t end, uint32_t budget) {
    const plot_model *m = g->model;
    const struct summary *s = &m->tree[node];
    uint32_t stop = end < m->count ? end : m->count, mid, i;
    double xmin, xmax, ymin, ymax, extent, vertical_extent;
    unsigned outside;
    if (start >= stop)
        return;
    g->stats.nodes++;
    if (!s->valid) {
        g->have = 0;
        return;
    }
    xmin = m->points[s->xmin].x;
    xmax = m->points[s->xmax].x;
    ymin = m->points[s->ymin].y;
    ymax = m->points[s->ymax].y;
    outside = xmax < g->view->xmin || xmin > g->view->xmax ||
              ymax < g->view->ymin || ymin > g->view->ymax;
    extent = (plot_axis_fraction(xmax, g->view->xmin, g->view->xmax) -
              plot_axis_fraction(xmin, g->view->xmin, g->view->xmax)) * g->width;
    vertical_extent = (plot_axis_fraction(ymax, g->view->ymin, g->view->ymax) -
                       plot_axis_fraction(ymin, g->view->ymin, g->view->ymax)) * g->width;
    if (outside || (extent < 1 && vertical_extent < 1) || budget < 12) {
        aggregate(g, s, start, stop);
        return;
    }
    if (node >= m->base) {
        if (stop - start > budget) {
            aggregate(g, s, start, stop);
            return;
        }
        for (i = start; i < stop; i++) {
            g->stats.points++;
            if (m->points[i].valid)
                emit(g, i, 1);
            else
                g->have = 0;
        }
        return;
    }
    mid = start + (end - start) / 2;
    /* Give offscreen/empty halves just enough for their connecting endpoints;
     * the visible sibling receives the rest. That makes zoom useful even in
     * very large monotone datasets without revisiting every source point. */
    {
        const struct summary *a = &m->tree[node * 2], *b = &m->tree[node * 2 + 1];
        uint32_t left = budget / 2, right;
        int ao = !a->valid || m->points[a->xmax].x < g->view->xmin ||
                 m->points[a->xmin].x > g->view->xmax;
        int bo = !b->valid || m->points[b->xmax].x < g->view->xmin ||
                 m->points[b->xmin].x > g->view->xmax;
        if (ao && !bo)
            left = 6;
        else if (bo && !ao)
            left = budget - 6;
        right = budget - left;
        visit(g, node * 2, start, mid, left);
        visit(g, node * 2 + 1, mid, end, right);
    }
}
uint32_t plot_model_geometry(const plot_model *m, const struct plot_view *v,
                             unsigned width, struct plot_vertex *out,
                             uint32_t capacity, struct plot_stats *stats) {
    struct geometry g;
    uint32_t budget = (uint32_t)width * 4 + 64;
    memset(&g, 0, sizeof(g));
    if (stats)
        memset(stats, 0, sizeof(*stats));
    if (!m || !m->ready || !m->count || !out || !plot_view_valid(v) || capacity < 6 || !width)
        return 0;
    g.model = m;
    g.view = v;
    g.out = out;
    g.width = width;
    g.capacity = capacity < budget ? capacity : budget;
    visit(&g, 1, 0, m->base * BLOCK, g.capacity);
    g.stats.vertices = g.count;
    if (stats)
        *stats = g.stats;
    return g.count;
}
static void xml_text(FILE *f, const char *text) {
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    while (*p) {
        unsigned c = *p++;
        if (c == '&')
            fputs("&amp;", f);
        else if (c == '<')
            fputs("&lt;", f);
        else if (c == '>')
            fputs("&gt;", f);
        else if (c == '"')
            fputs("&quot;", f);
        else if (c == '\'')
            fputs("&apos;", f);
        else if (c >= 32 || c == '\t' || c == '\n' || c == '\r')
            fputc(c, f);
    }
}
static void svg_tick(FILE *f, double value, double x, double y, const char *anchor) {
    fprintf(f, "<text x=\"%.2f\" y=\"%.2f\" text-anchor=\"%s\">%.5g</text>\n",
            x, y, anchor, value == 0 ? 0.0 : value);
}
static int export_error(char *error, unsigned capacity, const char *text) {
    if (capacity)
        snprintf(error, capacity, "%s", text);
    return -1;
}
struct plot_export_job {
    FILE *file;
    struct plot_vertex *vertices;
    struct plot_view view;
    char *temporary, *path;
    char error[160];
    uint32_t count, at;
    unsigned width, height, mode, done, failed;
};
void plot_export_delete(plot_export_job *job) {
    if (!job)
        return;
    if (job->file)
        fclose(job->file);
    if (job->temporary && *job->temporary && !job->done)
        unlink(job->temporary);
    free(job->temporary);
    free(job->path);
    free(job->vertices);
    free(job);
}
plot_export_job *plot_export_begin(const plot_model *m, const struct plot_view *v,
                                    unsigned mode, const char *xlabel, const char *ylabel,
                                    const char *path, unsigned width, unsigned height,
                                    char *error, unsigned capacity) {
    plot_export_job *job;
    uint32_t i, limit;
    FILE *f;
    int fd = -1;
    unsigned attempt, w, h;
    if (!m || !m->ready || !plot_view_valid(v) || !m->valid || !path || !*path ||
        width < 240 || width > 4096 || height < 160 || height > 4096 || mode > PLOT_SCATTER ||
        (xlabel && strlen(xlabel) > 4096) || (ylabel && strlen(ylabel) > 4096)) {
        export_error(error, capacity, "No plot to export, or invalid image size/labels.");
        return NULL;
    }
    limit = (uint32_t)width * 4 + 64;
    job = calloc(1, sizeof(*job));
    if (!job) {
        export_error(error, capacity, "Not enough memory to export the plot.");
        return NULL;
    }
    job->vertices = malloc(limit * sizeof(*job->vertices));
    job->temporary = malloc(strlen(path) + 48);
    job->path = malloc(strlen(path) + 1);
    if (job->temporary)
        job->temporary[0] = 0;
    if (!job->vertices || !job->temporary || !job->path) {
        plot_export_delete(job);
        export_error(error, capacity, "Not enough memory to export the plot.");
        return NULL;
    }
    strcpy(job->path, path);
    /* Clear the buffer until a temporary file has actually been created. */
    job->temporary[0] = 0;
    job->view = *v;
    job->width = width;
    job->height = height;
    job->mode = mode;
    job->count = plot_model_geometry(m, v, width, job->vertices, limit, NULL);
    for (attempt = 0; attempt < 100; attempt++) {
        snprintf(job->temporary, strlen(path) + 48, "%s.plot-%lu-%u.tmp", path,
                 (unsigned long)getpid(), attempt);
        fd = open(job->temporary, O_CREAT | O_EXCL | O_WRONLY, 0600);
        if (fd >= 0)
            break;
        if (errno != EEXIST)
            break;
    }
    f = fd < 0 ? NULL : fdopen(fd, "w");
    if (!f) {
        if (fd >= 0)
            close(fd);
        else
            job->temporary[0] = 0;
        plot_export_delete(job);
        export_error(error, capacity, "Cannot create the exported image.");
        return NULL;
    }
    job->file = f;
    w = width - 94;
    h = height - 90;
    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
               "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%u\" height=\"%u\" "
               "viewBox=\"0 0 %u %u\">\n"
               "<rect width=\"100%%\" height=\"100%%\" fill=\"#191a1d\"/>\n"
               "<g font-family=\"sans-serif\" font-size=\"12\" fill=\"#b2b4bd\">\n",
            width, height, width, height);
    for (i = 0; i < 5; i++) {
        double t = i / 4.0;
        fprintf(f, "<path d=\"M %.2f 36 V %u M 74 %.2f H %u\" "
                   "fill=\"none\" stroke=\"#34363d\"/>\n",
                74 + t * w, 36 + h, 36 + (1 - t) * h, 74 + w);
        svg_tick(f, plot_axis_value(v->xmin, v->xmax, t), 74 + t * w, 54 + h, "middle");
        svg_tick(f, plot_axis_value(v->ymin, v->ymax, t), 66, 40 + (1 - t) * h, "end");
    }
    fprintf(f, "<text x=\"%u\" y=\"%u\" text-anchor=\"middle\">", 74 + w / 2, height - 8);
    xml_text(f, xlabel);
    fputs("</text>\n<text x=\"74\" y=\"18\">", f);
    xml_text(f, ylabel);
    fputs("</text></g>\n<g fill=\"#7299ef\" stroke=\"#7299ef\" stroke-width=\"1.5\">\n", f);
    if (ferror(f)) {
        plot_export_delete(job);
        export_error(error, capacity, "Could not write the exported image.");
        return NULL;
    }
    if (capacity)
        error[0] = 0;
    return job;
}
int plot_export_step(plot_export_job *job, uint32_t budget) {
    uint32_t stop;
    unsigned w, h;
    FILE *f;
    if (!job || job->failed)
        return -1;
    if (job->done)
        return 1;
    if (!budget)
        return 0;
    f = job->file;
    w = job->width - 94;
    h = job->height - 90;
    stop = job->count - job->at;
    if (stop > budget)
        stop = budget;
    stop += job->at;
    for (; job->at < stop; job->at++) {
        struct plot_vertex *p = &job->vertices[job->at];
        double x0, y0, x1, y1;
        if (job->mode == PLOT_LINE && job->at && p->connect &&
            plot_clip_segment(&job->view, job->vertices[job->at - 1].x,
                              job->vertices[job->at - 1].y, p->x, p->y,
                              &x0, &y0, &x1, &y1))
            fprintf(f, "<path fill=\"none\" d=\"M %.3f %.3f L %.3f %.3f\"/>\n",
                    74 + x0 * w, 36 + (1 - y0) * h, 74 + x1 * w, 36 + (1 - y1) * h);
        if ((job->mode == PLOT_SCATTER || !p->connect) && !outcode(&job->view, p->x, p->y))
            fprintf(f, "<circle cx=\"%.3f\" cy=\"%.3f\" r=\"2\" stroke=\"none\"/>\n",
                    74 + plot_axis_fraction(p->x, job->view.xmin, job->view.xmax) * w,
                    36 + (1 - plot_axis_fraction(p->y, job->view.ymin, job->view.ymax)) * h);
    }
    if (ferror(f))
        goto bad;
    if (job->at != job->count)
        return 0;
    fputs("</g></svg>\n", f);
    if (ferror(f))
        goto bad;
    job->file = NULL;
    if (fclose(f))
        goto bad;
    if (rename(job->temporary, job->path))
        goto bad;
    job->done = 1;
    return 1;
bad:
    if (job->file) {
        fclose(job->file);
        job->file = NULL;
    }
    unlink(job->temporary);
    job->failed = 1;
    snprintf(job->error, sizeof(job->error), "%s", "Could not finish the exported image; the original was preserved.");
    return -1;
}
unsigned plot_export_progress(const plot_export_job *job) {
    return !job ? 0 : job->done ? 1000 : job->count ? (unsigned)(job->at * 1000UL / job->count) : 0;
}
const char *plot_export_error(const plot_export_job *job) {
    return job ? job->error : "No export job.";
}
int plot_export_svg(const plot_model *m, const struct plot_view *v, unsigned mode,
                     const char *xlabel, const char *ylabel, const char *path,
                     unsigned width, unsigned height, char *error, unsigned capacity) {
    plot_export_job *job = plot_export_begin(m, v, mode, xlabel, ylabel, path,
                                             width, height, error, capacity);
    int result;
    if (!job)
        return -1;
    do {
        result = plot_export_step(job, 64);
    } while (!result);
    if (result < 0)
        export_error(error, capacity, plot_export_error(job));
    plot_export_delete(job);
    return result < 0 ? -1 : 0;
}
