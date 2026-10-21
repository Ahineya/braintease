/* SPDX-License-Identifier: MIT */
/* Exercise the shared table's viewport invariants with UBSan, independently of
 * the viewer. Include the implementation to inspect bounded header recycling. */
#include "ui-data.c"
#include "src/core/lv_obj_private.h"
#include <assert.h>
static const char *value(void *user, uint32_t row, unsigned column) {
    (void)user;
    (void)row;
    (void)column;
    return "Ukrainian: Київ, Ελληνικά";
}
static lv_obj_t *heading(ui_table *t, unsigned col) {
    unsigned i;
    for (i = 0; i < t->pool_cols; i++)
        if (t->header_columns[i] == col && !lv_obj_is_hidden(t->headers[i]))
            return t->headers[i];
    return NULL;
}
static void settled(ui_table *t) {
    unsigned i;
    lv_area_t area;
    lv_obj_update_layout(t->root);
    table_viewport(t);
    lv_obj_update_layout(t->root);
    lv_refr_now(NULL);
    lv_obj_get_content_coords(t->head, &area);
    assert(lv_obj_get_scroll_x(t->head) == lv_obj_get_scroll_x(t->body));
    assert(t->corner->coords.x1 == area.x1);
    for (i = 0; i < t->pool_cols; i++) {
        lv_obj_t *h = t->headers[i];
        if (lv_obj_is_hidden(h))
            continue;
        assert(h->coords.x1 ==
               area.x1 + column_x(t, t->header_columns[i]) - lv_obj_get_scroll_x(t->body));
        assert(lv_obj_get_width(h) == gx_lv_px(t->widths[t->header_columns[i]]));
        assert(t->header_versions[i] == t->header_revision);
    }
    assert(t->pool_cols <= POOL_COLS);
}
static bool overlap_glyph(const lv_font_t *font, lv_font_glyph_dsc_t *g, uint32_t c,
                          uint32_t next) {
    (void)font;
    (void)c;
    (void)next;
    g->adv_w = 1;
    g->box_w = g->box_h = 2;
    g->stride = 2;
    g->format = LV_FONT_GLYPH_FORMAT_A8;
    return true;
}
static const void *overlap_pixels(lv_font_glyph_dsc_t *g, lv_draw_buf_t *buffer) {
    static const uint8_t pixels[] = {255, 255, 255, 255};
    (void)g;
    (void)buffer;
    return pixels;
}
static void cache_checks(ui_table *t) {
    static const lv_font_t overlap = {.get_glyph_dsc = overlap_glyph,
                                      .get_glyph_bitmap = overlap_pixels,
                                      .line_height = 2,
                                      .static_bitmap = 1};
    struct text_bitmap b = {0};
    uint32_t used = t->mask_cells, limit = t->mask_limit;
    unsigned i;
    text_bounds(&b, "Hello123", ui_font(UI_BODY));
    assert(bitmap_cache(t, &b, "Hello123", ui_font(UI_BODY)));
    assert(b.mask && t->mask_cells > used && t->mask_cells <= limit);
    bitmap_free(t, &b);
    assert(t->mask_cells == used);
    /* Overlapping alpha samples need sequential blending and must use LVGL. */
    text_bounds(&b, "AA", &overlap);
    assert(!bitmap_cache(t, &b, "AA", &overlap) && !b.mask);
    assert(t->mask_cells == used);
    bitmap_free(t, &b);
    t->mask_limit = 0;
    text_bounds(&b, "Hello123", ui_font(UI_BODY));
    assert(!bitmap_cache(t, &b, "Hello123", ui_font(UI_BODY)) && !b.mask);
    assert(t->mask_cells == used);
    bitmap_free(t, &b);
    t->mask_limit = 24000;
    ui_table_refresh(t);
    for (i = 0; i < 64; i++) {
        ui_table_select(t, i * 1000UL, i, 1);
        settled(t);
        assert(t->mask_cells <= t->mask_limit);
    }
    table_bitmaps_clear(t);
    assert(!t->mask_cells);
    t->mask_limit = limit;
    ui_table_refresh(t);
}
static unsigned disabled_selections, disabled_sorts;
static unsigned key_calls;
static int handled_key(void *user, uint32_t key) {
    assert(!user);
    key_calls++;
    return key == LV_KEY_DOWN;
}
static void key_policy_checks(ui_table *t) {
    uint32_t key = LV_KEY_DOWN;
    assert(ui_table_input(t) == t->body && !ui_table_input(NULL));
    ui_table_select(t, 4, 2, 1);
    ui_table_keys(t, handled_key);
    key_calls = 0;
    lv_obj_send_event(ui_table_input(t), LV_EVENT_KEY, &key);
    assert(key_calls == 1 && ui_table_row(t) == 4);
    key = LV_KEY_UP;
    lv_obj_send_event(ui_table_input(t), LV_EVENT_KEY, &key);
    assert(key_calls == 2 && ui_table_row(t) == 3);
    ui_table_enabled(t, 0);
    lv_obj_send_event(ui_table_input(t), LV_EVENT_KEY, &key);
    assert(key_calls == 2 && ui_table_row(t) == 3);
    ui_table_enabled(t, 1);
    ui_table_keys(t, NULL);
    key = LV_KEY_DOWN;
    lv_obj_send_event(ui_table_input(t), LV_EVENT_KEY, &key);
    assert(key_calls == 2 && ui_table_row(t) == 4);
}
static lv_indev_data_t pointer_sample;
static void disabled_selected(void *user, uint32_t row, unsigned column) {
    (void)user;
    (void)row;
    (void)column;
    disabled_selections++;
}
static void disabled_sorted(void *user, unsigned column) {
    (void)user;
    (void)column;
    disabled_sorts++;
}
static void pointer_read(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    *data = pointer_sample;
}
static void pointer_click(lv_indev_t *indev, int32_t x, int32_t y) {
    memset(&pointer_sample, 0, sizeof(pointer_sample));
    pointer_sample.point.x = x;
    pointer_sample.point.y = y;
    lv_indev_read(indev);
    pointer_sample.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(indev);
    pointer_sample.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(indev);
}
static void disabled_checks(ui_table *t) {
    lv_indev_t *pointer = lv_indev_create();
    lv_area_t body;
    uint32_t key = LV_KEY_DOWN;
    int32_t sx, sy;
    unsigned before;
    assert(pointer);
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_mode(pointer, LV_INDEV_MODE_EVENT);
    lv_indev_set_read_cb(pointer, pointer_read);
    ui_table_bind(t, 32, 64);
    ui_table_select(t, 0, 0, 1);
    settled(t);
    ui_table_callbacks(t, disabled_selected, disabled_sorted);
    disabled_selections = disabled_sorts = 0;
    t->dragging = 1;
    ui_table_enabled(t, 0);
    assert(!t->dragging && !lv_obj_is_scrollable(t->body));
    assert(lv_obj_has_state(t->body, LV_STATE_DISABLED));
    lv_obj_send_event(t->body, LV_EVENT_KEY, &key);
    lv_obj_send_event(heading(t, 0), LV_EVENT_CLICKED, NULL);
    lv_obj_get_content_coords(t->body, &body);
    pointer_click(pointer, body.x1 + gx_lv_px(NUMBER_WIDTH + 10),
                  body.y1 + gx_lv_px(UI_ROW_HEIGHT * 2 + 8));
    assert(ui_table_row(t) == 0 && ui_table_column(t) == 0);
    assert(!disabled_selections && !disabled_sorts);
    /* Disabled concerns user input: model commits may still bind, select and
     * reveal through the public API before reenabling the table. */
    ui_table_select(t, 8, 2, 1);
    assert(ui_table_row(t) == 8 && ui_table_column(t) == 2 && disabled_selections == 1);
    ui_table_bind(t, 64, 4);
    ui_table_select(t, 20, 3, 1);
    settled(t);
    assert(ui_table_row(t) == 20 && ui_table_column(t) == 3);
    assert(!lv_obj_is_scrollable(t->body) && lv_obj_has_state(t->body, LV_STATE_DISABLED));
    sx = lv_obj_get_scroll_x(t->body);
    sy = lv_obj_get_scroll_y(t->body);
    memset(&pointer_sample, 0, sizeof(pointer_sample));
    pointer_sample.point.x = body.x1 + gx_lv_px(100);
    pointer_sample.point.y = body.y1 + gx_lv_px(100);
    pointer_sample.enc_diff = -48;
    lv_indev_read(pointer);
    pointer_sample.enc_diff = 48;
    lv_indev_read(pointer);
    assert(lv_obj_get_scroll_x(t->body) == sx && lv_obj_get_scroll_y(t->body) == sy);
    ui_table_enabled(t, 1);
    assert(lv_obj_is_scrollable(t->body) && !lv_obj_has_state(t->body, LV_STATE_DISABLED));
    before = disabled_selections;
    lv_obj_send_event(t->body, LV_EVENT_KEY, &key);
    assert(ui_table_row(t) == 21 && disabled_selections == before + 1);
    ui_table_select(t, 0, 0, 1);
    settled(t);
    pointer_click(pointer, body.x1 + gx_lv_px(NUMBER_WIDTH + 10),
                  body.y1 + gx_lv_px(UI_ROW_HEIGHT * 2 + 8));
    assert(ui_table_row(t) == 2);
    lv_obj_send_event(heading(t, 0), LV_EVENT_CLICKED, NULL);
    assert(disabled_sorts == 1);
    /* A wheel event is ignored while disabled and works again after enable. */
    ui_table_select(t, 20, 0, 1);
    settled(t);
    sy = lv_obj_get_scroll_y(t->body);
    pointer_sample.enc_diff = -48;
    lv_indev_read(pointer);
    assert(lv_obj_get_scroll_y(t->body) != sy);
    lv_indev_delete(pointer);
    ui_table_callbacks(t, NULL, NULL);
    ui_table_bind(t, 100000, 64);
    settled(t);
}
int main(void) {
    ui_table *t;
    unsigned i, scale;
    assert(!gx_lv_open("Table component checks", 800, 560));
    ui_screen();
    t = ui_table_create(lv_screen_active(), value, NULL);
    lv_obj_set_size(t->root, gx_lv_px(760), gx_lv_px(400));
    for (i = 0; i < 64; i++)
        ui_table_heading(t, i, "Column heading");
    ui_table_bind(t, 100000, 64);
    for (scale = 1; scale <= 2; scale++) {
        gx_lv_apply_density(scale);
        lv_obj_set_size(t->root, gx_lv_px(760), gx_lv_px(400));
        ui_table_density(t);
        key_policy_checks(t);
        disabled_checks(t);
        for (i = 0; i < 64; i++)
            ui_table_column_width(t, i, 320);
        lv_obj_scroll_to(t->body, 0, 0, LV_ANIM_OFF);
        settled(t);
        /* Pixel scrolling must neither invalidate layout nor replace headers. */
        {
            lv_obj_t *first = heading(t, 0), *second = heading(t, 1);
            for (i = 1; i <= 30; i++) {
                lv_obj_scroll_to_x(t->body, gx_lv_px(i), LV_ANIM_OFF);
                assert(!lv_screen_active()->scr_layout_inv);
                assert(heading(t, 0) == first && heading(t, 1) == second);
                settled(t);
            }
            lv_obj_scroll_to_x(t->body, gx_lv_px(330), LV_ANIM_OFF);
            settled(t);
            assert(!heading(t, 0) && heading(t, 1) == second);
        }
        /* Scroll across/recycle every column, including the right boundary. */
        for (i = 0; i < 64; i++) {
            ui_table_select(t, i * 1000UL, i, 1);
            settled(t);
            assert(heading(t, i));
        }
        /* Hidden cached headings must pick up edits, width, sort and density. */
        ui_table_heading(t, 0, "Changed");
        ui_table_column_width(t, 0, 480);
        ui_table_sort_marker(t, 0, 1);
        ui_table_select(t, 0, 0, 1);
        settled(t);
        assert(!strcmp(lv_label_get_text(lv_obj_get_child(heading(t, 0), 0)), "Changed  v"));
        assert(lv_obj_has_state(heading(t, 0), LV_STATE_CHECKED));
        ui_table_select(t, 99999, 63, 1);
        settled(t);
        /* Rebinding a short/empty document resets both scroll axes and gutter. */
        ui_table_bind(t, 2, 1);
        settled(t);
        assert(!lv_obj_get_scroll_x(t->body) && !lv_obj_get_scroll_y(t->body));
        ui_table_bind(t, 0, 0);
        settled(t);
        for (i = 0; i < t->pool_cols; i++)
            assert(lv_obj_is_hidden(t->headers[i]));
        ui_table_bind(t, 100000, 64);
        for (i = 0; i < 64; i++)
            ui_table_column_width(t, i, 64);
        ui_table_select(t, 99999, 63, 1);
        settled(t);
        ui_table_select(t, 0, 0, 1);
        settled(t);
    }
    gx_lv_apply_density(1);
    lv_obj_set_size(t->root, gx_lv_px(760), gx_lv_px(400));
    ui_table_density(t);
    for (i = 0; i < 64; i++)
        ui_table_column_width(t, i, 320);
    ui_table_select(t, 100, 1, 1);
    lv_obj_scroll_to(t->body, 30, 2800, LV_ANIM_OFF);
    settled(t);
    for (scale = 2; scale; scale--) {
        gx_lv_apply_density(scale);
        lv_obj_set_size(t->root, gx_lv_px(760), gx_lv_px(400));
        ui_table_density(t);
        settled(t);
        assert(lv_obj_get_scroll_x(t->body) == gx_lv_px(30));
        assert(lv_obj_get_scroll_y(t->body) == gx_lv_px(2800));
    }
    lv_obj_set_size(t->root, gx_lv_px(1920), gx_lv_px(1080));
    ui_table_density(t);
    for (i = 0; i < 64; i++)
        ui_table_column_width(t, i, 64);
    ui_table_select(t, 0, 0, 1);
    settled(t);
    assert(t->pool_cols >= 30 && heading(t, 28));
    ui_table_select(t, 99999, 63, 1);
    settled(t);
    assert(heading(t, 63));
    cache_checks(t);
    gx_lv_close();
    puts("TABLE_COMPONENT passed: pixel scroll without layout; stable bounded headers; "
         "both densities; resizing/sorting; hidden cache edits; document replacement; bounded text "
         "bitmaps; overlap/low-memory fallback; disabled input, scrolling and API model updates");
    return 0;
}
