/* SPDX-License-Identifier: MIT */
#include "plot.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <math.h>
#include <unistd.h>

static void prepared(plot_model *m) {
    unsigned steps = 0;
    while (!plot_model_prepare_step(m, 7))
        assert(++steps < 10000);
    assert(plot_model_ready(m));
    assert(plot_model_append(m, 0, 0, 0, 1) < 0);
}
static int has_index(const struct plot_vertex *v, uint32_t count, uint32_t index) {
    uint32_t i;
    for (i = 0; i < count; i++)
        if (v[i].index == index)
            return 1;
    return 0;
}
static void large_geometry(void) {
    plot_model *m = plot_model_create(100000);
    struct plot_view view;
    struct plot_stats stats;
    struct plot_vertex *vertices = malloc(4096UL * sizeof(*vertices));
    uint32_t i, n;
    assert(m && vertices);
    for (i = 0; i < 100000; i++) {
        double y = i == 70001 ? 1000 : i == 40001 ? -900 : sin(i * 0.01);
        assert(!plot_model_append(m, i, y, i * 7 + 13, i < 48000 || i >= 52000));
    }
    assert(plot_model_count(m) == 100000 && plot_model_invalid(m) == 4000);
    assert(!plot_model_fit(m, &view));
    prepared(m);
    assert(plot_model_fit(m, &view));
    assert(view.ymin < -900 && view.ymax > 1000);
    n = plot_model_geometry(m, &view, 800, vertices, 4096, &stats);
    assert(n <= 3264 && n == stats.vertices);
    assert(stats.points < 10000 && stats.nodes < 10000);
    assert(has_index(vertices, n, 70001) && has_index(vertices, n, 40001));
    for (i = 0; i < n; i++) {
        assert(vertices[i].row == vertices[i].index * 7 + 13);
        if (i && vertices[i].connect) {
            assert(vertices[i - 1].index < vertices[i].index);
            assert(!(vertices[i - 1].index < 48000 && vertices[i].index >= 52000));
        }
    }
    /* A zoomed-in query resolves original points rather than retaining the
     * fit-view decimation. Panning work is still bounded by the viewport. */
    view.xmin = 70000;
    view.xmax = 70002;
    view.ymin = -2;
    view.ymax = 1001;
    n = plot_model_geometry(m, &view, 800, vertices, 4096, &stats);
    assert(has_index(vertices, n, 70001));
    assert(stats.points < 1000 && stats.nodes < 500);
    assert(plot_view_pan(&view, 0.1, -0.1));
    assert(plot_view_zoom(&view, 0.5, 0.5, 0.5));
    assert(!plot_view_zoom(&view, 0, 0.5, 0.5));
    plot_model_delete(m);
    free(vertices);
}
static void adversarial_gaps(void) {
    plot_model *m = plot_model_create(100000);
    struct plot_view view;
    struct plot_stats stats;
    struct plot_vertex vertices[192];
    uint32_t i, count;
    assert(m);
    for (i = 0; i < 100000; i++)
        assert(!plot_model_append(m, (i * 3571UL) % 100003UL, i == 99998 ? 9000 : i % 19,
                                 i + 100, !(i % 2)));
    prepared(m);
    assert(plot_model_fit(m, &view));
    count = plot_model_geometry(m, &view, 32, vertices, 192, &stats);
    assert(count <= 192 && stats.points < 1000 && stats.nodes < 1000);
    assert(has_index(vertices, count, 99998));
    for (i = 0; i < count; i++) {
        assert(!(vertices[i].index % 2));
        assert(!vertices[i].connect);
    }
    plot_model_delete(m);
    /* A vertical scatter distribution must not collapse to only two extrema
     * simply because all samples occupy the same x coordinate. */
    m = plot_model_create(10000);
    assert(m);
    for (i = 0; i < 10000; i++)
        assert(!plot_model_append(m, 1, i, i, 1));
    prepared(m);
    assert(plot_model_fit(m, &view));
    count = plot_model_geometry(m, &view, 32, vertices, 192, &stats);
    assert(count > 20 && count <= 192);
    plot_model_delete(m);
}
/* The demo's 40 temperatures reproduce a reported last-to-first diagonal after
 * panning. Check connected, clipped segments rather than just fit geometry:
 * offscreen endpoint representatives are allowed, but may not close a series
 * or bridge an invalid source sample when they re-enter the viewport. */
static void partial_views(const char *folder) {
    const double temperatures[] = {
        17.1, 11.2, 18.3, 12.4, 19.5, 13.6, 20.7, 14.8, 21.9, 15.0,
        22.1, 16.2, 10.3, 17.4, 11.5, 18.6, 12.7, 19.8, 13.9, 20.0,
        14.1, 21.2, 15.3, 22.4, 16.5, 10.6, 17.7, 11.8, 18.9, 12.0,
        19.1, 13.2, 20.3, 14.4, 21.5, 15.6, 22.7, 16.8, 10.9, 17.0
    };
    const struct plot_view views[] = {
        {-3.544, 38.19, 12.32, 25.59}, /* reported pan: bottom/right clipping */
        {8.25, 26.75, 14.15, 20.35},  /* clipping at every viewport edge */
        {32.25, 39.75, 13.0, 21.0},  /* entire first hierarchy leaf offscreen */
        {-12.0, -1.0, 12.0, 24.0},  /* all data offscreen */
        {1.0, 40.0, 17.5, 17.6}     /* narrow band, alternating crossings */
    };
    struct plot_vertex vertices[320];
    struct plot_stats stats;
    unsigned gap, k;
    for (gap = 0; gap < 2; gap++) {
        plot_model *m = plot_model_create(40);
        uint32_t i;
        assert(m);
        for (i = 0; i < 40; i++)
            assert(!plot_model_append(m, i + 1, temperatures[i], i * 7 + 13,
                                      !gap || (i != 4 && i != 17 && i != 32)));
        prepared(m);
        for (k = 0; k < sizeof(views) / sizeof(views[0]); k++) {
            struct plot_view view = views[k];
            uint32_t n = plot_model_geometry(m, &view, 256, vertices, 320, &stats);
            unsigned expected = 0, actual = 0;
            double x0, y0, x1, y1;
            for (i = 1; i < 40; i++) {
                const struct plot_point *a = plot_model_point(m, i - 1);
                const struct plot_point *b = plot_model_point(m, i);
                if (a->valid && b->valid &&
                    plot_clip_segment(&view, a->x, a->y, b->x, b->y,
                                      &x0, &y0, &x1, &y1))
                    expected++;
            }
            for (i = 0; i < n; i++) {
                assert(vertices[i].row == vertices[i].index * 7 + 13);
                assert(!i || vertices[i - 1].index < vertices[i].index);
                assert(i || !vertices[i].connect);
                if (i && vertices[i].connect &&
                    plot_clip_segment(&view, vertices[i - 1].x, vertices[i - 1].y,
                                      vertices[i].x, vertices[i].y, &x0, &y0, &x1, &y1)) {
                    assert(vertices[i].index == vertices[i - 1].index + 1);
                    assert(x1 >= x0);
                    assert(x1 - x0 <= 1.0 / (view.xmax - view.xmin) + 1e-12);
                    actual++;
                }
            }
            assert(actual == expected);
            if (!gap && !k) {
                char path[512], error[160], line[512];
                FILE *file;
                unsigned segments = 0;
                snprintf(path, sizeof(path), "%s/plot-partial.svg", folder);
                assert(!plot_export_svg(m, &view, PLOT_LINE, "Row", "Temperature",
                                        path, 800, 500, error, sizeof(error)));
                file = fopen(path, "r");
                assert(file);
                while (fgets(line, sizeof(line), file)) {
                    char *at = strstr(line, "<path fill=\"none\" d=\"M ");
                    double sx0, sx1;
                    if (!at)
                        continue;
                    at += strlen("<path fill=\"none\" d=\"M ");
                    sx0 = strtod(at, &at);
                    strtod(at, &at);
                    assert(*at == ' ' && at[1] == 'L');
                    sx1 = strtod(at + 2, &at);
                    assert(sx1 >= sx0);
                    assert(sx1 - sx0 <= 706.0 / (view.xmax - view.xmin) + 0.002);
                    segments++;
                }
                assert(!fclose(file));
                assert(segments == expected);
            }
        }
        plot_model_delete(m);
    }
}
static void domains(void) {
    const double values[] = {0, 1, -1, DBL_MAX, -DBL_MAX, DBL_MIN, 1e-310};
    unsigned i;
    double x0, y0, x1, y1;
    struct plot_view view = {-1, 1, -1, 1};
    for (i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        plot_model *m = plot_model_create(2);
        assert(m && !plot_model_append(m, values[i], values[i], 71, 1));
        prepared(m);
        assert(plot_model_fit(m, &view));
        assert(plot_view_valid(&view));
        assert(view.xmin <= values[i] && view.xmax >= values[i]);
        assert(plot_axis_fraction(values[i], view.xmin, view.xmax) >= 0);
        assert(plot_axis_fraction(values[i], view.xmin, view.xmax) <= 1);
        plot_model_delete(m);
    }
    view.xmin = view.ymin = -1;
    view.xmax = view.ymax = 1;
    /* Check the saturated return paths explicitly on the C16 ABI as well as
     * the host: a non-double 1 return used to become the double bit pattern 1
     * and send a clipped right/top endpoint to the opposite plot edge. */
    assert(plot_axis_fraction(-2.0, -1.0, 1.0) == 0.0);
    assert(plot_axis_fraction(-1.0, -1.0, 1.0) == 0.0);
    assert(plot_axis_fraction(1.0, -1.0, 1.0) == 1.0);
    assert(plot_axis_fraction(2.0, -1.0, 1.0) == 1.0);
    assert(plot_axis_fraction(0.0, -1.0, 1.0) == 0.5);
    /* Closely spaced large offsets must retain the data's available bits.
     * Normalizing the bounds near +/-1 before subtracting loses enough bits
     * here to move a quarter-axis point by several physical pixels. */
    assert(plot_axis_fraction(1e200 - 1e185, 1e200 - 2e185, 1e200 + 2e185) == 0.25);
    assert(plot_axis_fraction(1e200 + 1e185, 1e200 - 2e185, 1e200 + 2e185) == 0.75);
    assert(plot_axis_fraction(-1e200 - 1e185, -1e200 - 2e185, -1e200 + 2e185) == 0.25);
    assert(plot_axis_fraction(-1e200 + 1e185, -1e200 - 2e185, -1e200 + 2e185) == 0.75);
    assert(plot_clip_segment(&view, -DBL_MAX, -DBL_MAX, DBL_MAX, DBL_MAX,
                            &x0, &y0, &x1, &y1));
    assert(x0 == 0 && y0 == 0 && x1 == 1 && y1 == 1);
    assert(plot_clip_segment(&view, -10, 0, 10, 0, &x0, &y0, &x1, &y1));
    assert(x0 == 0 && y0 == 0.5 && x1 == 1 && y1 == 0.5);
    assert(!plot_clip_segment(&view, -2, 3, 2, 3, &x0, &y0, &x1, &y1));
    {
        plot_model *m = plot_model_create(3);
        assert(m);
        assert(!plot_model_append(m, -DBL_MAX, -DBL_MAX, 0, 1));
        assert(!plot_model_append(m, DBL_MAX, DBL_MAX, 1, 1));
        assert(!plot_model_append(m, HUGE_VAL, 0, 2, 1));
        prepared(m);
        assert(plot_model_invalid(m) == 1 && plot_model_fit(m, &view));
        assert(plot_axis_fraction(0, view.xmin, view.xmax) == 0.5);
        assert(!plot_view_pan(&view, 1, 0));
        plot_model_delete(m);
    }
    {
        plot_model *m = plot_model_create(0);
        assert(m);
        prepared(m);
        assert(!plot_model_fit(m, &view) && !plot_model_valid(m));
        plot_model_delete(m);
    }
    assert(!plot_model_create(PLOT_MAX_POINTS + 1));
}
static void export_svg(const char *folder) {
    plot_model *m = plot_model_create(1200);
    struct plot_view view;
    plot_export_job *job;
    char path[512], error[160], *text;
    FILE *file;
    long length;
    uint32_t i;
    snprintf(path, sizeof(path), "%s/plot-model.svg", folder);
    for (i = 0; i < 1200; i++)
        assert(!plot_model_append(m, i, i == 700 ? 30 : sin(i * 0.01), i, i < 480 || i > 520));
    prepared(m);
    assert(plot_model_fit(m, &view));
    file = fopen(path, "w");
    assert(file);
    fputs("existing document", file);
    assert(!fclose(file));
    job = plot_export_begin(m, &view, PLOT_LINE, "Time <s> & \"clock\"", "A > B's value", path,
                              800, 500, error, sizeof(error));
    assert(job && !*error);
    assert(!plot_export_step(job, 1) && plot_export_progress(job) < 1000);
    plot_export_delete(job);
    file = fopen(path, "r");
    assert(file);
    assert(fgetc(file) == 'e');
    fclose(file);
    job = plot_export_begin(m, &view, PLOT_SCATTER, "Time <s> & \"clock\"", "A > B's value", path,
                              800, 500, error, sizeof(error));
    assert(job);
    i = 0;
    while (!plot_export_step(job, 11))
        assert(++i < 10000);
    assert(plot_export_progress(job) == 1000 && !*plot_export_error(job));
    plot_export_delete(job);
    file = fopen(path, "r");
    assert(file && !fseek(file, 0, SEEK_END));
    length = ftell(file);
    assert(length > 500 && !fseek(file, 0, SEEK_SET));
    text = malloc((uint32_t)length + 1);
    assert(text && fread(text, 1, (uint32_t)length, file) == (uint32_t)length);
    text[length] = 0;
    fclose(file);
    assert(strstr(text, "Time &lt;s&gt; &amp; &quot;clock&quot;"));
    assert(strstr(text, "A &gt; B&apos;s value"));
    assert(strstr(text, "<circle") && strstr(text, "</svg>"));
    assert(!strstr(text, "nan") && !strstr(text, "inf"));
    free(text);
    assert(!plot_export_svg(m, &view, PLOT_LINE, "x", "y", path, 800, 500, error, sizeof(error)));
    assert(plot_export_svg(m, &view, PLOT_LINE, "x", "y", folder, 800, 500, error, sizeof(error)) < 0);
    assert(*error);
    plot_model_delete(m);
}
int main(int argc, char **argv) {
    partial_views(argc > 1 ? argv[1] : "/tmp");
    domains();
    large_geometry();
    adversarial_gaps();
    export_svg(argc > 1 ? argv[1] : "/tmp");
    puts("PLOT_MODEL passed: bounded hierarchy/geometry; original IDs; peaks and gaps; "
         "zoom detail; finite extremes; partial-view clipping and SVG; "
         "XML and transactional/cancellable SVG");
    return 0;
}
