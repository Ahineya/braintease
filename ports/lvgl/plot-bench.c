/* SPDX-License-Identifier: MIT */
/* Test-only: continuous pointer drag through the production plot widget and
 * Fuse-X raster/upload/presentation path, without acceptance PPM capture I/O. */
#include "ui-plot.c"
#include <assert.h>
#include <time.h>
static lv_point_t bench_pointer;
static unsigned bench_pressed;
static void bench_input(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    data->point = bench_pointer;
    data->state = bench_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
static uint32_t bench_clock(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)now.tv_sec * 1000000UL + now.tv_nsec / 1000UL;
}
static int compare_time(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}
int main(int argc, char **argv) {
    unsigned width = argc > 1 ? atoi(argv[1]) : 800, height = argc > 2 ? atoi(argv[2]) : 560;
    uint32_t rows = argc > 3 ? strtoul(argv[3], NULL, 10) : 40;
    unsigned frames = argc > 4 ? atoi(argv[4]) : 32, events = argc > 5 ? atoi(argv[5]) : 1;
    plot_model *model;
    ui_plot *plot;
    lv_indev_t *indev;
    lv_area_t all, area;
    struct plot_stats stats;
    uint32_t *times, total, input_time = 0, prepare_time = 0, mask_time = 0, refresh_time = 0;
    uint32_t input_max = 0, prepare_max = 0, mask_max = 0, refresh_max = 0, at, frame_at, duration, i;
    unsigned j;
    assert(rows && rows <= 100000 && frames >= 10 && frames <= 300 && events && events <= 40);
    model = plot_model_create(rows);
    times = malloc(frames * sizeof(*times));
    assert(model && times);
    for (i = 0; i < rows; i++)
        assert(!plot_model_append(model, i, (double)((i * 3571UL) % 100003UL) / 1000, i, 1));
    while (!plot_model_prepare_step(model, 4096)) {}
    assert(!gx_lv_open("Plot drag benchmark", width, height));
    ui_screen();
    plot = ui_plot_create(lv_screen_active());
    assert(plot);
    gx_lv_pos(ui_plot_object(plot), 16, 16);
    gx_lv_size(ui_plot_object(plot), width - 32, height - 32);
    ui_plot_labels(plot, "Sample", "Measurement");
    ui_plot_bind(plot, model);
    lv_obj_update_layout(ui_plot_object(plot));
    lv_refr_now(NULL);
    assert(plot_area(plot, &all, &area));
    indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, bench_input);
    bench_pointer.x = (area.x1 + area.x2) / 2;
    bench_pointer.y = (area.y1 + area.y2) / 2;
    bench_pressed = 1;
    lv_indev_read(indev);
    lv_refr_now(NULL);
    total = bench_clock();
    for (i = 0; i < frames; i++) {
        frame_at = at = bench_clock();
        for (j = 0; j < events; j++) {
            unsigned step = i * events + j;
            int32_t dx = (int32_t)(step % 40) - 20;
            int32_t dy = (int32_t)(step % 20) - 10;
            bench_pointer.x = (area.x1 + area.x2) / 2 + gx_lv_px(dx * 3);
            bench_pointer.y = (area.y1 + area.y2) / 2 + gx_lv_px(dy);
            lv_indev_read(indev);
        }
        duration = bench_clock() - at;
        input_time += duration;
        if (duration > input_max) input_max = duration;
        at = bench_clock();
        prepare(plot, &area);
        duration = bench_clock() - at;
        prepare_time += duration;
        if (duration > prepare_max) prepare_max = duration;
        at = bench_clock();
#ifdef GRAPHITE_PLOT_RASTER_H
        curve_prepare(plot, (uint32_t)(area.x2 - area.x1 + 1),
                       (uint32_t)(area.y2 - area.y1 + 1));
#endif
        duration = bench_clock() - at;
        mask_time += duration;
        if (duration > mask_max) mask_max = duration;
        at = bench_clock();
        lv_refr_now(NULL);
        duration = bench_clock() - at;
        refresh_time += duration;
        if (duration > refresh_max) refresh_max = duration;
        times[i] = bench_clock() - frame_at;
    }
    total = bench_clock() - total;
    ui_plot_stats(plot, &stats);
    qsort(times, frames, sizeof(*times), compare_time);
    printf("PLOT_BENCH frames=%u events_per_frame=%u rows=%lu width=%u height=%u density=%u "
           "wall_us=%lu input_us=%lu prepare_us=%lu mask_us=%lu refresh_us=%lu "
           "p50_us=%lu p95_us=%lu p99_us=%lu max_frame_us=%lu max_input_us=%lu "
           "max_prepare_us=%lu max_mask_us=%lu max_refresh_us=%lu nodes=%lu points=%lu vertices=%lu primitives=%lu\n",
           frames, events, (unsigned long)rows, width, height, gx_lv_density(),
           (unsigned long)total, (unsigned long)input_time, (unsigned long)prepare_time,
           (unsigned long)mask_time, (unsigned long)refresh_time, (unsigned long)times[frames / 2],
           (unsigned long)times[(frames * 95 - 1) / 100], (unsigned long)times[(frames * 99 - 1) / 100],
           (unsigned long)times[frames - 1], (unsigned long)input_max,
           (unsigned long)prepare_max, (unsigned long)mask_max, (unsigned long)refresh_max,
           (unsigned long)stats.nodes, (unsigned long)stats.points,
           (unsigned long)stats.vertices, (unsigned long)plot->painted_count);
    bench_pressed = 0;
    lv_indev_read(indev);
    ui_plot_bind(plot, NULL);
    plot_model_delete(model);
    lv_obj_delete(ui_plot_object(plot));
    free(times);
    gx_lv_close();
    return 0;
}
