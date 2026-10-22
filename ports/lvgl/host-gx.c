/* SPDX-License-Identifier: MIT */
/* Test-only byte-addressed rendering reference for the exact calculator UI. */
#include "ui.h"
#include "ui-data.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static lv_display_t *display;
static lv_group_t *group;
static unsigned width, height, flushes;
static int window_active = 1;
static uint8_t *frame;
static uint8_t *draw;
static uint32_t now;
static int (*on_key)(unsigned, int, unsigned);
static void (*on_resize)(unsigned, unsigned);
void gx_lv_application(const char *app_id, const char *name, const char *icon) {
    (void)app_id;
    (void)name;
    (void)icon;
}
int gx_lv_open_panel(const char *title, uint32_t authority, unsigned *w, unsigned *h) {
    (void)authority;
    if (!*w) *w = 1024;
    if (!*h) *h = 768;
    return gx_lv_open(title, *w, *h);
}
int gx_lv_fd(void) { return -1; }
int gx_lv_panel_alpha(void) { return 0; }
uint32_t gx_lv_window(void) { return 0; }
int gx_lv_active(void) { return window_active; }
/* Focused native tests drive the same window-active predicate as GX_FOCUS. */
void gx_lv_host_active(int active) { window_active = active != 0; }
void gx_lv_idle(void (*poll)(void)) { (void)poll; }
void gx_lv_events(int (*filter)(const struct gx_event *)) { (void)filter; }
void gx_lv_cancel_key_repeat(void) {}
static lv_obj_t *find_widget(lv_obj_t *obj, const lv_obj_class_t *type, const char *label) {
    unsigned i;
    if (lv_obj_check_type(obj, type) && (!label || !strcmp(lv_label_get_text(obj), label)))
        return obj;
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *found = find_widget(lv_obj_get_child(obj, i), type, label);
        if (found)
            return found;
    }
    return NULL;
}
static uint32_t tick(void) { return now; }
void gx_lv_minimum(unsigned w, unsigned h) {
    (void)w;
    (void)h;
}
void gx_lv_shortcuts(int (*key)(unsigned, int, unsigned)) { on_key = key; }
void gx_lv_finish(void) {}
void gx_lv_close_request(int (*request)(void)) { (void)request; }
void gx_lv_release_mask(void *base) { (void)base; }
int gx_lv_raster_a8(uint8_t *dst, long w, long h, long stride,
                    const struct gx_a8_primitive *primitives, unsigned long count) {
    (void)dst;
    (void)w;
    (void)h;
    (void)stride;
    (void)primitives;
    (void)count;
    return 0;
}
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *map) {
    uint32_t stride = lv_display_get_buf_active(d)->header.stride;
    int x, y;
    for (y = a->y1; y <= a->y2; y++)
        for (x = a->x1; x <= a->x2; x++) {
            uint8_t *p = map + (y - a->y1) * stride + (x - a->x1) * 3;
            uint8_t *out = frame + (y * width + x) * 3;
            out[0] = p[2];
            out[1] = p[1];
            out[2] = p[0];
        }
    flushes++;
    lv_display_flush_ready(d);
}
int gx_lv_open(const char *title, unsigned w, unsigned h) {
    unsigned scale = getenv("LVGL_SCALE") && !strcmp(getenv("LVGL_SCALE"), "2") ? 2 : 1;
    (void)title;
    window_active = 1;
    width = w * scale;
    height = h * scale;
    lv_init();
    lv_tick_set_cb(tick);
    frame = calloc(width * height * 3, sizeof(*frame));
    draw = malloc(1024 * 16 * 3);
    assert(frame && draw);
    display = lv_display_create(width, height);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB888);
    lv_display_set_buffers(display, draw, NULL, 1024 * 16 * 3, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    gx_lv_apply_density(scale);
    group = lv_group_create();
    lv_group_set_default(group);
    return 0;
}
lv_group_t *gx_lv_group(void) { return group; }
void gx_lv_set_group(lv_group_t *next) {
    group = next;
    lv_group_set_default(next);
    if (window_active)
        ui_focus_resume(group);
    else
        ui_focus_suspend(group);
}
void gx_lv_handlers(void (*text)(uint32_t), void (*action)(unsigned),
                    void (*resize)(unsigned, unsigned)) {
    (void)text;
    (void)action;
    on_resize = resize;
}
int gx_lv_clipboard(char *text, unsigned capacity, int write) {
    (void)text;
    (void)capacity;
    (void)write;
    return -1;
}
int gx_lv_run(void) {
    unsigned i, before;
    FILE *out = fopen(getenv("LVCALC_REFERENCE"), "wb");
    assert(out);
    if (getenv("LVGL_REFERENCE_PAGE") && on_key)
        on_key(58 + atoi(getenv("LVGL_REFERENCE_PAGE")), 1, 0);
    if (getenv("LVGL_REFERENCE_DIALOG")) {
        lv_obj_t *label = find_widget(lv_screen_active(), &lv_label_class, "Actions");
        lv_obj_t *menu, *input;
        assert(label);
        lv_obj_update_layout(lv_screen_active());
        lv_obj_send_event(lv_obj_get_parent(label), LV_EVENT_CLICKED, NULL);
        menu = lv_obj_get_child(lv_obj_get_child(lv_layer_top(), 0), 0);
        lv_obj_send_event(lv_obj_get_child(menu, 3), LV_EVENT_CLICKED, NULL);
        input = find_widget(lv_layer_top(), &lv_textarea_class, NULL);
        assert(input);
        lv_textarea_set_text(input, "Таблиця π!");
        ui_data_resize();
    }
    if (getenv("LVGL_REFERENCE_CHECKED")) {
        /* Gallery's content panel is nested: find its live checkbox by type/text. */
        lv_obj_t *stack[128];
        unsigned count = 0;
        stack[count++] = lv_screen_active();
        while (count) {
            lv_obj_t *obj = stack[--count];
            uint32_t n;
            if (lv_obj_check_type(obj, &lv_checkbox_class) &&
                !strcmp(lv_checkbox_get_text(obj), "Show hidden files"))
                lv_obj_add_state(obj, LV_STATE_CHECKED);
            for (n = 0; n < lv_obj_get_child_count(obj); n++) {
                assert(count < 128);
                stack[count++] = lv_obj_get_child(obj, n);
            }
        }
    }
    if (getenv("LVGL_REFERENCE_FILE_DIALOG") && on_key) {
        int save = !strcmp(getenv("LVGL_REFERENCE_FILE_DIALOG"), "save");
        on_key(save ? 22 : 18, 1, save ? 3 : 1);
        on_key(save ? 22 : 18, 0, save ? 3 : 1);
    }
    /* Finish incremental document/plot jobs and textarea caret animations
     * before comparing a settled reference with the real-time guest window.
     * This advances only the deterministic test clock, with no wall delay. */
    for (i = 0; i < 1000; i++) {
        now += 20;
        lv_timer_handler();
    }
    if ((getenv("LVGL_REFERENCE_FULLSCREEN") || getenv("LVGL_REFERENCE_SIZE")) && on_resize) {
        unsigned scale = gx_lv_density(), w = 1024, h = 1024;
        if (getenv("LVGL_REFERENCE_SIZE"))
            assert(sscanf(getenv("LVGL_REFERENCE_SIZE"), "%ux%u", &w, &h) == 2);
        width = w * scale;
        height = h * scale;
        free(frame);
        frame = calloc((uint32_t)width * height * 3, sizeof(*frame));
        assert(frame);
        lv_display_set_resolution(display, width, height);
        on_resize(w, h);
        for (i = 0; i < 50; i++) {
            now += 20;
            lv_timer_handler();
        }
    }
    if (getenv("LVGL_REFERENCE_END") && on_key) {
        on_key(77, 1, 1);
        for (i = 0; i < 50; i++) {
            now += 20;
            lv_timer_handler();
        }
    }
    if (getenv("LVGL_REFERENCE_RIGHT") && on_key) {
        for (i = 0; i < (unsigned)atoi(getenv("LVGL_REFERENCE_RIGHT")); i++)
            lv_group_send_data(group, LV_KEY_RIGHT);
        for (i = 0; i < 50; i++) {
            now += 20;
            lv_timer_handler();
        }
    }
    lv_refr_now(display);
    if (getenv("LVGL_REFERENCE_DIALOG") || getenv("LVGL_REFERENCE_FILE_DIALOG")) {
        char path[1024];
        lv_area_t a;
        FILE *geometry;
        lv_obj_t *dialog = lv_obj_get_child(lv_obj_get_child(lv_layer_top(), 0), 0);
        lv_obj_get_coords(dialog, &a);
        snprintf(path, sizeof(path), "%s.modal.json", getenv("LVCALC_REFERENCE"));
        geometry = fopen(path, "w");
        assert(geometry);
        fprintf(geometry, "[%ld,%ld,%ld,%ld]\n", (long)a.x1, (long)a.y1, (long)a.x2, (long)a.y2);
        assert(!fclose(geometry));
    }
    before = flushes;
    for (i = 0; i < 100; i++)
        lv_timer_handler();
    assert(flushes == before);
    fprintf(out, "P6\n%u %u\n255\n", width, height);
    assert(fwrite(frame, 3, width * height, out) == width * height);
    assert(!fclose(out));
    puts("LVGL_REFERENCE idle_flushes=0");
    return 0;
}
void gx_lv_close(void) {
    ui_theme_close();
    lv_deinit();
    free(frame);
    free(draw);
}
