/* SPDX-License-Identifier: MIT */
/* A byte-addressed host reference. This is not a Fuse-X or Riplix GUI port. */
#include "lvgl.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define WIDTH 640
#define HEIGHT 400
static uint16_t frame[WIDTH * HEIGHT];
static uint16_t draw_buffer[WIDTH * 32];
static unsigned flushes;
static unsigned long pixels_flushed;

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *map)
{
    unsigned y;
    unsigned width = (unsigned)(area->x2 - area->x1 + 1);
    unsigned stride = lv_display_get_buf_active(display)->header.stride;
    assert(area->x1 >= 0 && area->y1 >= 0 && area->x2 < WIDTH && area->y2 < HEIGHT);
    for (y = 0; y <= (unsigned)(area->y2 - area->y1); y++)
        memcpy(frame + (area->y1 + y) * WIDTH + area->x1, map + y * stride,
               width * sizeof(uint16_t));
    flushes++;
    pixels_flushed += width * (area->y2 - area->y1 + 1);
    lv_display_flush_ready(display);
}

static void write_image(const char *path)
{
    FILE *file = fopen(path, "wb");
    unsigned i;
    assert(file);
    fprintf(file, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
    for (i = 0; i < WIDTH * HEIGHT; i++) {
        uint16_t p = frame[i];
        fputc(((p >> 11) & 31) * 255 / 31, file);
        fputc(((p >> 5) & 63) * 255 / 63, file);
        fputc((p & 31) * 255 / 31, file);
    }
    assert(fclose(file) == 0);
}

int main(int argc, char **argv)
{
    unsigned i, before;
    unsigned long initial_pixels, edit_pixels;
    int32_t x[] = {0, 1, 3, 6, 10};
    int32_t y[] = {0, 1, 9, 36, 100};
    lv_init();
    lv_display_t *display = lv_display_create(WIDTH, HEIGHT);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x15212b), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xe5edf2), 0);
    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Syma worksheet | LVGL reference");
    lv_obj_set_pos(title, 16, 12);
    lv_obj_t *editor = lv_textarea_create(screen);
    lv_obj_set_pos(editor, 16, 42);
    lv_obj_set_size(editor, 608, 64);
    lv_obj_set_style_bg_color(editor, lv_color_hex(0x21323f), 0);
    lv_obj_set_style_text_color(editor, lv_color_hex(0xe5edf2), 0);
    lv_obj_set_style_border_color(editor, lv_color_hex(0x4d6476), 0);
    lv_obj_set_style_border_width(editor, 1, 0);
    lv_obj_set_style_pad_all(editor, 8, 0);
    lv_textarea_set_text(editor, "{Define $squares {Map {Power _ 2} $samples}}\n{LinePlot $squares}");
    lv_obj_t *table = lv_table_create(screen);
    lv_obj_set_pos(table, 16, 126);
    lv_obj_set_size(table, 210, 230);
    lv_obj_set_style_bg_color(table, lv_color_hex(0x21323f), LV_PART_MAIN);
    lv_obj_set_style_bg_color(table, lv_color_hex(0x233947), LV_PART_ITEMS);
    lv_obj_set_style_text_color(table, lv_color_hex(0xe5edf2), LV_PART_ITEMS);
    lv_obj_set_style_pad_all(table, 7, LV_PART_ITEMS);
    lv_table_set_column_count(table, 2);
    lv_table_set_column_width(table, 0, 95);
    lv_table_set_column_width(table, 1, 95);
    lv_table_set_cell_value(table, 0, 0, "x");
    lv_table_set_cell_value(table, 0, 1, "x squared");
    for (i = 0; i < 5; i++) {
        lv_table_set_cell_value_fmt(table, i + 1, 0, "%ld", (long)x[i]);
        lv_table_set_cell_value_fmt(table, i + 1, 1, "%ld", (long)y[i]);
    }
    lv_obj_t *chart = lv_chart_create(screen);
    lv_obj_set_pos(chart, 258, 144);
    lv_obj_set_size(chart, 350, 196);
    lv_obj_set_style_bg_color(chart, lv_color_hex(0x21323f), 0);
    lv_obj_set_style_line_color(chart, lv_color_hex(0x4d6476), LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 1, LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    lv_chart_set_type(chart, LV_CHART_TYPE_SCATTER);
    lv_chart_set_point_count(chart, 5);
    lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_X, 0, 10);
    lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_series_t *series = lv_chart_add_series(chart, lv_color_hex(0x44ccd4), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_series_ext_x_array(chart, series, x);
    lv_chart_set_series_ext_y_array(chart, series, y);
    lv_obj_t *caption = lv_label_create(screen);
    lv_label_set_text(caption, "Nonuniform X samples | 0..10 / 0..100");
    lv_obj_set_pos(caption, 258, 360);
    lv_obj_update_layout(screen);
    lv_refr_now(display);
    initial_pixels = pixels_flushed;
    assert(initial_pixels >= WIDTH * HEIGHT);
    if (argc > 1) write_image(argv[1]);

    before = flushes;
    /* No changes or elapsed tick: no pixel flush should occur. */
    for (i = 0; i < 100; i++) lv_timer_handler();
    assert(flushes == before);
    lv_table_set_cell_value(table, 2, 1, "changed");
    lv_refr_now(display);
    edit_pixels = pixels_flushed - initial_pixels;
    assert(edit_pixels > 0 && edit_pixels < WIDTH * HEIGHT);

    /* UTF-8 storage is preserved; deletion operates on code points, not
     * extended graphemes. A notebook editor should keep Rift's text model. */
    lv_textarea_set_text(editor, "e\xcc\x81");
    lv_textarea_set_cursor_pos(editor, LV_TEXTAREA_CURSOR_LAST);
    lv_textarea_delete_char(editor);
    assert(strcmp(lv_textarea_get_text(editor), "e") == 0);
    printf("LVGL_HOST initial_pixels=%lu edited_pixels=%lu idle_flushes=0 "
           "table_rows=%lu chart_points=5 combining_delete=codepoint\n",
           initial_pixels, edit_pixels, (unsigned long)lv_table_get_row_count(table));
    lv_display_delete(display);
    lv_deinit();
    return 0;
}
