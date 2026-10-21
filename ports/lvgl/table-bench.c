/* SPDX-License-Identifier: MIT */
/* Test-only: profile scrolling through the actual data viewer and CSV model. */
#define main viewer_application_main
#include "data-viewer.c"
#undef main
#include <time.h>
#include <assert.h>
static unsigned widget_count(lv_obj_t *obj) {
    unsigned i, count = 1;
    for (i = 0; i < lv_obj_get_child_count(obj); i++)
        count += widget_count(lv_obj_get_child(obj, i));
    return count;
}
static uint32_t elapsed_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)t.tv_sec * 1000000UL + t.tv_nsec / 1000UL;
}
int main(int argc, char **argv) {
    lv_obj_t *body;
    char error[200];
    unsigned i, j, horizontal = argc == 3 && !strcmp(argv[2], "horizontal");
    uint32_t start, at, frame_at, duration, max_frame = 0, max_layout = 0;
    uint32_t scroll = 0, layout = 0, render = 0;
    unsigned events = getenv("TABLE_BENCH_EVENTS") ? atoi(getenv("TABLE_BENCH_EVENTS")) : 1;
    unsigned column_width = getenv("TABLE_BENCH_WIDTH") ? atoi(getenv("TABLE_BENCH_WIDTH")) : 320;
    if (!events || events > 40 || (argc != 2 && !horizontal))
        return 1;
    if (getenv("TABLE_BENCH_SCREEN_WIDTH"))
        screen_width = atoi(getenv("TABLE_BENCH_SCREEN_WIDTH"));
    if (getenv("TABLE_BENCH_SCREEN_HEIGHT"))
        screen_height = atoi(getenv("TABLE_BENCH_SCREEN_HEIGHT"));
    settings_load();
    if (csv_load(&document, argv[1], 0, 1, error, sizeof(error)))
        return 1;
    if (gx_lv_open("Table scroll benchmark", screen_width, screen_height))
        return 1;
    ui();
    strcpy(current_path, argv[1]);
    bind_document();
    if (horizontal)
        for (i = 0; i < document.columns; i++)
            ui_table_column_width(table, i, column_width);
    body = lv_obj_get_child(ui_table_object(table), 1);
    lv_obj_update_layout(ui_table_object(table));
    lv_refr_now(NULL);
    assert(widget_count(ui_table_object(table)) == ui_table_widget_count(table));
    start = elapsed_us();
    for (i = 0; i < 30; i++) {
        frame_at = at = elapsed_us();
        for (j = 0; j < events; j++) {
            int32_t delta = (j + 1) * 40 / events - j * 40 / events;
            delta = gx_lv_px(i < 15 ? -delta : delta);
            lv_obj_scroll_by_bounded(body, horizontal ? delta : 0, horizontal ? 0 : delta,
                                     LV_ANIM_OFF);
        }
        scroll += elapsed_us() - at;
        at = elapsed_us();
        lv_obj_update_layout(ui_table_object(table));
        duration = elapsed_us() - at;
        layout += duration;
        if (duration > max_layout)
            max_layout = duration;
        at = elapsed_us();
        lv_refr_now(NULL);
        render += elapsed_us() - at;
        duration = elapsed_us() - frame_at;
        if (duration > max_frame)
            max_frame = duration;
    }
    assert(widget_count(ui_table_object(table)) == ui_table_widget_count(table));
    printf("TABLE_BENCH frames=30 events_per_frame=%u rows=%lu widgets=%u wall_us=%lu "
           "scroll_us=%lu layout_us=%lu "
           "refresh_us=%lu max_frame_us=%lu max_layout_us=%lu\n",
           events, (unsigned long)document.row_count, ui_table_widget_count(table),
           (unsigned long)(elapsed_us() - start), (unsigned long)scroll, (unsigned long)layout,
           (unsigned long)render, (unsigned long)max_frame, (unsigned long)max_layout);
    gx_lv_close();
    csv_free(&document);
    return 0;
}
