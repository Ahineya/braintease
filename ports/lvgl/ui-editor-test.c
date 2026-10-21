/* SPDX-License-Identifier: MIT */
/* The viewport's private state is inspected for bounded ownership/layout
 * invariants; edits and navigation use its actual public interface. */
#include "ui-editor.c"
#include <assert.h>
#include <stdio.h>
static char clipboard[GX_CLIPBOARD_MAX + 1];
static unsigned notifications;
static lv_display_flush_cb_t original_flush;
static unsigned long flushed_pixels;
static unsigned char *captured;
static unsigned capture_width, capture_height;
static lv_point_t pointer_point;
static lv_indev_state_t pointer_state;
static void capture(lv_display_t *display, const lv_area_t *area, uint8_t *map) {
    unsigned stride = lv_display_get_buf_active(display)->header.stride;
    int32_t x, y;
    for (y = area->y1; y <= area->y2; y++)
        for (x = area->x1; x <= area->x2; x++) {
            const uint8_t *src = map + (y - area->y1) * stride + (x - area->x1) * 3;
            unsigned char *dst = captured + ((unsigned long)y * capture_width + x) * 3;
            dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
            flushed_pixels++;
        }
    original_flush(display, area, map);
}
static void pointer_read(lv_indev_t *pointer, lv_indev_data_t *data) {
    (void)pointer;
    data->point = pointer_point;
    data->state = pointer_state;
}
int gx_lv_clipboard(char *bytes, unsigned capacity, int write) {
    if (write) {
        if (strlen(bytes) > GX_CLIPBOARD_MAX)
            return -1;
        strcpy(clipboard, bytes);
    } else {
        if (strlen(clipboard) >= capacity)
            return -1;
        strcpy(bytes, clipboard);
    }
    return 0;
}
static void changed(lv_event_t *ev) { (void)ev; notifications++; }
static void finish(ui_editor *e) {
    unsigned count = 0;
    lv_obj_update_layout(lv_screen_active());
    editor_layout(e);
    while (!ui_editor_ready(e)) {
        step_layout(e, EDITOR_WORK);
        assert(++count < 1000);
    }
    lv_obj_update_layout(lv_screen_active());
}
static void expect(TextBuffer *b, const char *text) {
    unsigned long n = text_length(b);
    unsigned char *copy = malloc(n + 1);
    if (n != strlen(text))
        fprintf(stderr, "Expected <%s> (%lu octets), got %lu octets\n", text, (unsigned long)strlen(text), n);
    assert(copy && n == strlen(text));
    assert(!text_copy(b, 0, n, copy));
    copy[n] = 0;
    assert(!strcmp((char *)copy, text));
    free(copy);
}
static void key(ui_editor *e, unsigned code, unsigned modifiers) {
    assert(ui_editor_key(e, code, 1, modifiers));
    assert(ui_editor_key(e, code, 0, modifiers));
}
static void set(ui_editor *e, const char *text) {
    ui_editor_select(e, 0, text_length(e->buffer));
    assert(!ui_editor_text(e, text, strlen(text)));
    finish(e);
}
static void unicode_checks(ui_editor *e) {
    unsigned long bounds[2], p;
    int32_t x, y, h;
    const char *sample = "Aé é 👨‍👩‍👧‍👦 🇺🇦\r\nКиїв";
    set(e, sample);
    ui_editor_select(e, 0, 0);
    p = 0;
    while (p < text_length(e->buffer)) {
        unsigned long next = text_next(e->buffer, p);
        key(e, 79, 0);
        assert(ui_editor_cursor(e) == next);
        key(e, 80, 0);
        assert(ui_editor_cursor(e) == p);
        key(e, 79, 0);
        p = next;
    }
    for (p = 0; p < text_length(e->buffer); p = text_next(e->buffer, p)) {
        unsigned long hit;
        ui_editor_select(e, p, p);
        assert(ui_editor_caret(e, &x, &y, &h));
        hit = ui_editor_hit(e, x, y + h / 2);
        assert(hit == p);
    }
    ui_editor_select(e, 5, 8); /* A range through a combining cluster. */
    ui_editor_selection(e, bounds);
    assert(text_boundary(e->buffer, bounds[0]) == bounds[0]);
    assert(text_boundary(e->buffer, bounds[1]) == bounds[1]);
    set(e, "A\r\nB");
    ui_editor_select(e, 1, 1);
    key(e, 40, 0);
    expect(e->buffer, "A\r\n\r\nB");
    key(e, 29, 1);
    expect(e->buffer, "A\r\nB");
    set(e, "x é y");
    ui_editor_select(e, 5, 5);
    key(e, 42, 0);
    expect(e->buffer, "x  y");
    key(e, 29, 1);
    expect(e->buffer, "x é y");
}
static void clipboard_checks(ui_editor *e) {
    unsigned long revision, n;
    char *large;
    set(e, "prefix ");
    for (n = 0; n < 12000; n++)
        clipboard[n] = (n % 63 == 62) ? '\n' : 'a' + n % 26;
    clipboard[n] = 0;
    revision = text_revision(e->buffer);
    assert(ui_editor_action(e, GX_ACTION_PASTE));
    assert(text_length(e->buffer) == 12007);
    assert(text_revision(e->buffer) != revision);
    key(e, 29, 1);
    expect(e->buffer, "prefix ");
    key(e, 29, 3);
    assert(text_length(e->buffer) == 12007);
    ui_editor_select(e, 7, text_length(e->buffer));
    assert(ui_editor_action(e, GX_ACTION_COPY));
    assert(strlen(clipboard) == 12000);
    assert(ui_editor_action(e, GX_ACTION_CUT));
    expect(e->buffer, "prefix ");
    key(e, 29, 1);
    assert(text_length(e->buffer) == 12007);
    large = malloc(40001);
    assert(large);
    memset(large, 'x', 40000);
    large[40000] = 0;
    set(e, large);
    ui_editor_action(e, GX_ACTION_SELECT_ALL);
    ui_editor_action(e, GX_ACTION_CUT);
    assert(text_length(e->buffer) == 40000);
    assert(strstr(ui_editor_error(e), "32 KiB"));
    free(large);
}
static void geometry_checks(ui_editor *e) {
    unsigned long wrapped, n;
    int32_t narrow, wide, x, y, h;
    char *longline = malloc(1201);
    assert(longline);
    set(e, "iiiiWWWW");
    narrow = row_x(e, &e->rows[0], 4);
    wide = row_x(e, &e->rows[0], 8) - narrow;
    assert(wide > narrow); /* These are measured pixels, never fixed cells. */
    memset(longline, 'W', 1200);
    longline[1200] = 0;
    set(e, longline);
    assert(text_lines(e->buffer) == 1);
    assert(ui_editor_display_rows(e) > 10);
    wrapped = ui_editor_display_rows(e);
    lv_obj_set_width(e->root, gx_lv_px(650));
    finish(e);
    assert(ui_editor_display_rows(e) < wrapped);
    ui_editor_wrap(e, 0);
    finish(e);
    assert(ui_editor_display_rows(e) == 1);
    key(e, 77, 1);
    assert(ui_editor_cursor(e) == 1200);
    lv_obj_update_layout(e->root);
    assert(lv_obj_get_scroll_x(e->root) > gx_lv_px(1000));
    assert(ui_editor_caret(e, &x, &y, &h));
    assert(x >= 0 && x <= e->width);
    for (n = 0; n <= 1200; n += 200) {
        int32_t px = row_x(e, &e->rows[0], n) - lv_obj_get_scroll_x(e->root);
        assert(ui_editor_hit(e, px, 0) == n);
    }
    ui_editor_wrap(e, 1);
    finish(e);
    assert(ui_editor_display_rows(e) > 1);
    key(e, 74, 1);
    assert(!ui_editor_cursor(e));
    key(e, 77, 0);
    assert(ui_editor_caret(e, &x, &y, &h) && y == 0 && x > 0);
    key(e, 74, 0);
    assert(!ui_editor_cursor(e));
    key(e, 81, 2);
    assert(ui_editor_cursor(e) > 0 && !e->anchor);
    key(e, 79, 0);
    assert(e->cursor == e->anchor);
    free(longline);
}
static void pointer_damage_checks(ui_editor *e) {
    lv_indev_t *pointer = lv_indev_create();
    lv_area_t area;
    unsigned long bounds[2], before, first, last;
    unsigned char *incremental;
    int32_t x, y, h;
    assert(pointer);
    set(e, "A useful graphical editor\nКиїв, Львів, Одеса\nOne two three");
    ui_editor_select(e, 0, 0);
    lv_obj_update_layout(lv_screen_active());
    lv_obj_get_content_coords(e->root, &area);
    first = ui_editor_hit(e, gx_lv_px(8), e->line_height / 2);
    last = ui_editor_hit(e, gx_lv_px(115), e->line_height / 2);
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, pointer_read);
    ui_attach_pointer(pointer);
    pointer_point.x = area.x1 + gx_lv_px(8);
    pointer_point.y = area.y1 + e->line_height / 2;
    pointer_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(pointer);
    pointer_point.x = area.x1 + gx_lv_px(115);
    lv_indev_read(pointer);
    pointer_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(pointer);
    ui_editor_selection(e, bounds);
    assert(bounds[0] == first && bounds[1] == last && first < last);
    assert(lv_obj_is_scrollable(e->root) && !e->dragging);
    assert(!lv_obj_get_scroll_y(e->root));
    /* The GX adapter resets active input on focus loss/resize. LVGL sends
     * INDEV_RESET rather than RELEASED; scrolling and the caret must recover. */
    pointer_point.x = area.x1 + gx_lv_px(8);
    pointer_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(pointer);
    assert(e->dragging && !lv_obj_is_scrollable(e->root));
    lv_indev_reset(pointer, NULL);
    lv_indev_wait_release(pointer);
    pointer_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(pointer);
    assert(!e->dragging && lv_obj_is_scrollable(e->root));
    pointer_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(pointer);
    assert(e->dragging && !lv_obj_is_scrollable(e->root));
    ui_editor_buffer(e, e->buffer);
    assert(!e->dragging && lv_obj_is_scrollable(e->root));
    lv_indev_reset(pointer, NULL);
    pointer_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(pointer);
    lv_indev_delete(pointer);
    ui_editor_select(e, 5, 5);
    lv_refr_now(lv_display_get_default());
    before = flushed_pixels;
    key(e, 79, 0);
    lv_refr_now(lv_display_get_default());
    assert(flushed_pixels - before < (unsigned long)e->width * e->height / 4);
    incremental = malloc((unsigned long)capture_width * capture_height * 3);
    assert(incremental);
    memcpy(incremental, captured, (unsigned long)capture_width * capture_height * 3);
    lv_obj_invalidate(e->root);
    lv_refr_now(lv_display_get_default());
    assert(!memcmp(incremental, captured, (unsigned long)capture_width * capture_height * 3));
    free(incremental);
    assert(ui_editor_caret(e, &x, &y, &h));
    assert(ui_editor_hit(e, x, y + h / 2) == ui_editor_cursor(e));
}
static void syntax_checks(ui_editor *e) {
    unsigned i, kinds = 0;
    set(e, "int main(void) { /* comment */ return 42; }\nconst char *name = \"Київ\";\n");
    text_language(e->buffer, 1);
    ui_editor_changed(e);
    finish(e);
    prepare_paint(e);
    assert(e->paint_count >= 2);
    for (i = 0; i < e->paint[0].count; i++)
        kinds |= 1U << e->paint[0].runs[i].style;
    assert((kinds & (1U << 1)) && (kinds & (1U << 2)) && (kinds & (1U << 4)));
    lv_refr_now(lv_display_get_default());
}
static void work_checks(ui_editor *e) {
    TextBuffer *second = text_new();
    char *many = malloc(60001);
    unsigned i;
    assert(second && many);
    for (i = 0; i < 60000; i++)
        many[i] = i % 6 == 5 ? '\n' : 'x';
    many[60000] = 0;
    assert(!text_append(second, (const unsigned char *)many, 60000));
    text_saved(second);
    ui_editor_buffer(e, second);
    assert(!ui_editor_ready(e));
    assert(e->count < text_lines(second));
    finish(e);
    assert(ui_editor_display_rows(e) == 10001);
    assert(lv_obj_get_child_count(e->root) == 1);
    assert(!text_modified(second));
    ui_editor_buffer(e, NULL);
    assert(!ui_editor_caret(e, &e->goal, &e->width, &e->height));
    text_free(second); /* The viewport never owns or double-frees the model. */
    free(many);
}
int main(void) {
    TextBuffer *buffer = text_new();
    ui_editor *e;
    assert(buffer && !gx_lv_open("Editor component checks", 800, 560));
    capture_width = lv_display_get_horizontal_resolution(lv_display_get_default());
    capture_height = lv_display_get_vertical_resolution(lv_display_get_default());
    captured = calloc((unsigned long)capture_width * capture_height * 3, 1);
    assert(captured);
    original_flush = lv_display_get_flush_cb(lv_display_get_default());
    lv_display_set_flush_cb(lv_display_get_default(), capture);
    ui_screen();
    e = ui_editor_create(lv_screen_active(), buffer);
    assert(e);
    gx_lv_pos(e->root, 16, 16);
    gx_lv_size(e->root, 360, 470);
    lv_obj_add_event_cb(e->root, changed, LV_EVENT_VALUE_CHANGED, NULL);
    ui_editor_focus(e);
    finish(e);
    unicode_checks(e);
    clipboard_checks(e);
    geometry_checks(e);
    pointer_damage_checks(e);
    syntax_checks(e);
    work_checks(e);
    ui_editor_buffer(e, buffer);
    syntax_checks(e);
    assert(notifications > 40);
    assert(!gx_lv_run()); /* Includes the reference adapter's idle-flush check. */
    lv_obj_delete(e->root);
    assert(text_length(buffer));
    text_free(buffer);
    gx_lv_close();
    free(captured);
    puts("EDITOR_COMPONENT passed: UTF8/grapheme caret, pixel wrap/hit, grouped paste, undo/redo, syntax, bounded ownership and idle");
    return 0;
}
