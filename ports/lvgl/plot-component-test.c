/* SPDX-License-Identifier: MIT */
/* Inspect the same cached primitives that the guest renderer paints. */
#include "ui-plot.c"
#include <assert.h>
#include <math.h>
static uint32_t chosen = PLOT_NO_ROW;
static lv_point_t pointer;
static unsigned pressed;
static void input(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    data->point = pointer;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
static void selection(void *user, uint32_t row) {
    assert(user == &chosen);
    chosen = row;
}
static void text_invariants(ui_plot *p) {
    uint32_t cells = 0;
    unsigned i;
    for (i = 0; i < PLOT_TEXT_RUNS; i++) {
        struct plot_text *b = &p->labels[i];
        if (!b->mask)
            continue;
        assert(b->font && b->supported && b->image.data == b->mask);
        assert(b->image.header.cf == LV_COLOR_FORMAT_A8);
        assert(b->image.data_size == b->image.header.w * b->image.header.h);
        assert(b->image.header.stride == b->image.header.w);
        cells += b->image.data_size;
    }
    assert(cells == p->text_cells && cells <= PLOT_TEXT_LIMIT);
}
static void text_fallbacks(ui_plot *p) {
    struct plot_text temporary;
    uint8_t *mask;
    uint32_t before = p->text_cells;
    char longtext[300];
    memset(&temporary, 0, sizeof(temporary));
    assert(text_prepare(p, &temporary, "Cache12", ui_font(UI_SECONDARY), 4096));
    mask = temporary.mask;
    assert(text_prepare(p, &temporary, "Cache12", ui_font(UI_SECONDARY), 4096));
    assert(temporary.mask == mask);
    assert(!text_prepare(p, &temporary, "Cache12", ui_font(UI_SECONDARY), 1));
    assert(temporary.mask == mask);
    assert(text_prepare(p, &temporary, "Cache12", ui_font(UI_SECONDARY), 4096));
    assert(text_prepare(p, &temporary, "Cache34", ui_font(UI_SECONDARY), 4096));
    assert(!strcmp(temporary.text, "Cache34"));
    assert(text_prepare(p, &temporary, "Cache34", ui_font(UI_HEADING), 4096));
    assert(temporary.font == ui_font(UI_HEADING));
    assert(!text_prepare(p, &temporary, "one\ntwo", ui_font(UI_SECONDARY), 4096));
    memset(longtext, 'x', sizeof(longtext) - 1);
    longtext[sizeof(longtext) - 1] = 0;
    assert(!text_prepare(p, &temporary, longtext, ui_font(UI_SECONDARY), 4096));
    text_release(p, &temporary);
    assert(p->text_cells == before);
    /* Simulate exhaustion without depending on the host's allocator. A
     * refused run owns no buffer, and a fresh slot recovers when space exists. */
    p->text_cells = PLOT_TEXT_LIMIT;
    assert(!text_prepare(p, &temporary, "Budget1", ui_font(UI_SECONDARY), 4096));
    assert(!temporary.mask);
    p->text_cells = before;
    text_release(p, &temporary);
    assert(text_prepare(p, &temporary, "Budget1", ui_font(UI_SECONDARY), 4096));
    text_release(p, &temporary);
    assert(p->text_cells == before);
    text_invariants(p);
}
/* Compare the optimized inside/affine projection against the independent
 * robust clip-and-normalize path also used by SVG. Rounding can differ by one
 * physical pixel at a half-pixel boundary; topology and visibility cannot. */
static void reference_primitives(ui_plot *p, const lv_area_t *area) {
    uint32_t i, at = 0;
    int32_t width = area->x2 - area->x1, height = area->y2 - area->y1;
    for (i = 0; i < p->count; i++) {
        const struct plot_vertex *v = &p->vertices[i];
        struct gx_a8_primitive expected, *actual;
        double x0, y0, x1, y1;
        if (p->mode == PLOT_LINE && i && v->connect &&
            plot_clip_segment(&p->view, p->vertices[i - 1].x, p->vertices[i - 1].y,
                              v->x, v->y, &x0, &y0, &x1, &y1)) {
            expected.x0 = (int32_t)(x0 * width + 0.5);
            expected.y0 = (int32_t)((1 - y0) * height + 0.5);
            expected.x1 = (int32_t)(x1 * width + 0.5);
            expected.y1 = (int32_t)((1 - y1) * height + 0.5);
            expected.dot = 0;
            expected.size = gx_lv_px(1);
        } else if ((p->mode == PLOT_SCATTER || !v->connect) && inside(&p->view, v->x, v->y)) {
            expected.x0 = expected.x1 = (int32_t)(plot_axis_fraction(v->x, p->view.xmin,
                                                                  p->view.xmax) * width + 0.5);
            expected.y0 = expected.y1 = (int32_t)((1 - plot_axis_fraction(v->y, p->view.ymin,
                                                                       p->view.ymax)) * height + 0.5);
            expected.dot = 1;
            expected.size = gx_lv_px(2);
        } else
            continue;
        assert(at < p->painted_count);
        actual = &p->painted[at++];
        assert(actual->dot == expected.dot && actual->size == expected.size);
        if (abs(actual->x0 - expected.x0) > 1 || abs(actual->x1 - expected.x1) > 1 ||
            abs(actual->y0 - expected.y0) > 1 || abs(actual->y1 - expected.y1) > 1)
            fprintf(stderr, "PROJECTION mode=%u count=%lu i=%lu view=(%.17g,%.17g,%.17g,%.17g) "
                    "point=(%.17g,%.17g) actual=(%ld,%ld,%ld,%ld) expected=(%ld,%ld,%ld,%ld)\n",
                    p->mode, (unsigned long)p->count, (unsigned long)i,
                    p->view.xmin, p->view.xmax, p->view.ymin, p->view.ymax, v->x, v->y,
                    (long)actual->x0, (long)actual->y0, (long)actual->x1, (long)actual->y1,
                    (long)expected.x0, (long)expected.y0, (long)expected.x1, (long)expected.y1);
        assert(abs(actual->x0 - expected.x0) <= 1 && abs(actual->x1 - expected.x1) <= 1);
        assert(abs(actual->y0 - expected.y0) <= 1 && abs(actual->y1 - expected.y1) <= 1);
    }
    assert(at == p->painted_count);
}
static void settled(ui_plot *p) {
    lv_area_t all, area;
    uint32_t i;
    lv_obj_update_layout(p->root);
    lv_refr_now(NULL);
    text_invariants(p);
    if (!p->ready || !plot_area(p, &all, &area))
        return;
    assert(!p->dirty);
    assert(p->count <= (uint32_t)(area.x2 - area.x1) / gx_lv_density() * 4 + 64);
    assert(p->painted_count <= p->count);
    for (i = 0; i < p->painted_count; i++) {
        const struct gx_a8_primitive *v = &p->painted[i];
        assert(v->x0 >= 0 && v->x1 >= 0 && v->y0 >= 0 && v->y1 >= 0);
        assert(v->x0 <= area.x2 - area.x1 && v->x1 <= area.x2 - area.x1);
        assert(v->y0 <= area.y2 - area.y1 && v->y1 <= area.y2 - area.y1);
    }
    reference_primitives(p, &area);
}
static unsigned coverage(ui_plot *p) {
    uint32_t i;
    unsigned opaque = 0, partial = 0;
    assert(p->mask_ready && !p->mask_dirty && p->mask);
    assert(p->curve.data == p->mask && p->curve.header.cf == LV_COLOR_FORMAT_A8);
    assert(p->curve.data_size == p->curve.header.w * p->curve.header.h);
    assert(p->curve.header.stride == p->curve.header.w);
    assert(p->curve.data_size <= p->mask_capacity);
    for (i = 0; i < p->curve.data_size; i++) {
        opaque += p->mask[i] == 255;
        partial += p->mask[i] && p->mask[i] < 255;
    }
    if (p->painted_count)
        assert(opaque);
    else
        assert(!opaque && !partial);
    return partial;
}
static void domain_primitives(ui_plot *p) {
    unsigned domain, i, mode;
    for (domain = 0; domain < 5; domain++) {
        plot_model *m = plot_model_create(9);
        struct plot_view view;
        assert(m);
        for (i = 0; i < 9; i++) {
            double t = ((int)i - 4) / 4.0, x, y;
            if (domain == 0) {
                x = t * 4;
                y = sin(i * 0.7) * 2;
                view.xmin = -2;
                view.xmax = 2;
                view.ymin = -1.5;
                view.ymax = 1.5;
            } else if (domain == 1) {
                x = t * 1e-310;
                y = sin(i * 0.7) * 1e-310;
                view.xmin = view.ymin = -1e-310;
                view.xmax = view.ymax = 1e-310;
            } else if (domain == 2) {
                x = t * DBL_MAX;
                y = t * DBL_MAX;
                view.xmin = view.ymin = -DBL_MAX;
                view.xmax = view.ymax = DBL_MAX;
            } else if (domain == 3) {
                x = 1e200 + t * 4e185;
                y = 1e200 - t * 4e185;
                view.xmin = view.ymin = 1e200 - 2e185;
                view.xmax = view.ymax = 1e200 + 2e185;
            } else {
                x = t * DBL_MIN;
                y = sin(i * 0.7) * DBL_MIN;
                view.xmin = view.ymin = -DBL_MIN;
                view.xmax = view.ymax = DBL_MIN;
            }
            assert(!plot_model_append(m, x, y, i + 37, i != 5));
        }
        while (!plot_model_prepare_step(m, 7)) {}
        ui_plot_bind(p, m);
        assert(ui_plot_set_view(p, &view));
        for (mode = PLOT_LINE; mode <= PLOT_SCATTER; mode++) {
            ui_plot_mode(p, mode);
            settled(p);
            assert(p->count && p->painted_count);
        }
        ui_plot_bind(p, NULL);
        plot_model_delete(m);
    }
}
int main(void) {
    plot_model *m = plot_model_create(100000), *empty = plot_model_create(0);
    ui_plot *p;
    lv_indev_t *indev;
    struct plot_view fitted, moved;
    struct plot_stats stats;
    lv_area_t all, area;
    uint32_t i;
    unsigned scale;
    assert(m && empty && !gx_lv_open("Plot component checks", 800, 560));
    ui_screen();
    for (i = 0; i < 100000; i++)
        assert(!plot_model_append(m, i, i == 70001 ? 50 : sin(i * 0.001), i + 91,
                                 i < 48000 || i >= 52000));
    while (!plot_model_prepare_step(m, 50)) {}
    assert(plot_model_prepare_step(empty, 1));
    p = ui_plot_create(lv_screen_active());
    assert(p);
    gx_lv_size(p->root, 760, 500);
    settled(p);
    ui_plot_bind(p, empty);
    settled(p);
    assert(!ui_plot_get_view(p, &fitted) && !p->count && !p->painted_count);
    ui_plot_message(p, "No numeric values in this column.");
    settled(p);
    assert(!strcmp(p->message, "No numeric values in this column."));
    ui_plot_bind(p, m);
    ui_plot_labels(p, "Time (seconds)", "Measurements & value");
    ui_plot_callback(p, selection, &chosen);
    settled(p);
    assert(ui_plot_get_view(p, &fitted));
    assert(p->painted_count > 10);
    assert(coverage(p));
    ui_plot_stats(p, &stats);
    assert(stats.points < 10000 && stats.nodes < 10000);
    /* An ordinary repaint reuses both the hierarchy query and clipped screen
     * primitives; LVGL's partial strips never rebuild the viewport. */
    {
        struct plot_vertex *vertices = p->vertices;
        struct gx_a8_primitive *primitives = p->painted;
        uint8_t *mask = p->mask;
        uint8_t first = mask[0];
        uint8_t *labels[PLOT_TEXT_RUNS];
        unsigned j;
        char xnumbers[sizeof(p->xnumbers)], ynumbers[sizeof(p->ynumbers)];
        for (j = 0; j < PLOT_TEXT_RUNS; j++)
            labels[j] = p->labels[j].mask;
        memcpy(xnumbers, p->xnumbers, sizeof(xnumbers));
        memcpy(ynumbers, p->ynumbers, sizeof(ynumbers));
        /* Mark one cached coverage cell to detect needless rerasterization;
         * a normal invalidation must reuse its storage and contents. */
        mask[0] = first == 255 ? 17 : 255;
        for (i = 0; i < 10; i++) {
            lv_obj_invalidate(p->root);
            settled(p);
            assert(p->vertices == vertices && p->painted == primitives);
            assert(p->mask == mask && p->mask_ready && !p->mask_dirty);
            for (j = 0; j < PLOT_TEXT_RUNS; j++)
                assert(p->labels[j].mask == labels[j]);
            assert(mask[0] == (first == 255 ? 17 : 255));
            assert(!memcmp(xnumbers, p->xnumbers, sizeof(xnumbers)));
            assert(!memcmp(ynumbers, p->ynumbers, sizeof(ynumbers)));
            assert(!memcmp(&stats, &p->stats, sizeof(stats)));
        }
        ui_plot_select(p, 70001);
        settled(p);
        assert(p->mask == mask && mask[0] == (first == 255 ? 17 : 255));
        ui_plot_select(p, PLOT_NO_ROW);
        mask[0] = first;
    }
    ui_plot_focus(p);
    assert(ui_plot_focused(p));
    lv_group_send_data(gx_lv_group(), LV_KEY_RIGHT);
    assert(ui_plot_get_view(p, &moved) && moved.xmin > fitted.xmin);
    lv_group_send_data(gx_lv_group(), '+');
    assert(ui_plot_get_view(p, &moved) && moved.xmax - moved.xmin < fitted.xmax - fitted.xmin);
    lv_group_send_data(gx_lv_group(), LV_KEY_HOME);
    assert(ui_plot_get_view(p, &moved) && !memcmp(&moved, &fitted, sizeof(moved)));
    ui_plot_select(p, 70001);
    assert(ui_plot_selected(p) == 70092);
    settled(p);
    ui_plot_select(p, 50000);
    assert(ui_plot_selected(p) == PLOT_NO_ROW);
    /* Pointer selection goes back to the source row, not a decimation bucket. */
    indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, input);
    lv_obj_update_layout(p->root);
    assert(plot_area(p, &all, &area));
    pointer.x = area.x1 + (int32_t)(plot_axis_fraction(70001, fitted.xmin, fitted.xmax) *
                                    (area.x2 - area.x1));
    pointer.y = area.y2 - (int32_t)(plot_axis_fraction(50, fitted.ymin, fitted.ymax) *
                                    (area.y2 - area.y1));
    pressed = 1;
    lv_indev_read(indev);
    pressed = 0;
    lv_indev_read(indev);
    assert(chosen == 70092 && ui_plot_selected(p) == 70092);
    /* A drag pans from the press snapshot, and does not select a point. */
    pointer.x = (area.x1 + area.x2) / 2;
    pointer.y = (area.y1 + area.y2) / 2;
    pressed = 1;
    lv_indev_read(indev);
    pointer.x += gx_lv_px(40);
    pointer.y += gx_lv_px(20);
    lv_indev_read(indev);
    pressed = 0;
    lv_indev_read(indev);
    assert(ui_plot_get_view(p, &moved) && moved.xmin < fitted.xmin);
    ui_plot_fit(p);
    /* Disabled plots still paint and resize, but cannot mutate the view or
     * select a source row through a stale focus or held pointer. */
    lv_obj_add_state(p->root, LV_STATE_DISABLED);
    chosen = PLOT_NO_ROW;
    ui_plot_select(p, PLOT_NO_ROW);
    ui_plot_focus(p);
    lv_group_send_data(gx_lv_group(), LV_KEY_RIGHT);
    lv_group_send_data(gx_lv_group(), '+');
    assert(ui_plot_get_view(p, &moved) && !memcmp(&moved, &fitted, sizeof(moved)));
    pointer.x = area.x1 + (int32_t)(plot_axis_fraction(70001, fitted.xmin, fitted.xmax) *
                                    (area.x2 - area.x1));
    pointer.y = area.y2 - (int32_t)(plot_axis_fraction(50, fitted.ymin, fitted.ymax) *
                                    (area.y2 - area.y1));
    pressed = 1;
    lv_indev_read(indev);
    pointer.x += gx_lv_px(40);
    lv_indev_read(indev);
    pressed = 0;
    lv_indev_read(indev);
    assert(chosen == PLOT_NO_ROW && ui_plot_selected(p) == PLOT_NO_ROW);
    assert(ui_plot_get_view(p, &moved) && !memcmp(&moved, &fitted, sizeof(moved)));
    lv_obj_remove_state(p->root, LV_STATE_DISABLED);
    for (scale = 1; scale <= 2; scale++) {
        gx_lv_apply_density(scale);
        ui_plot_density(p);
        text_fallbacks(p);
        /* Tick separation follows each axis independently. A wide, short
         * worksheet output cannot inherit the X axis's five Y labels. */
        assert(vertical_ticks(gx_lv_px(40)) == 1);
        assert(vertical_ticks(gx_lv_px(80)) == 2);
        assert(vertical_ticks(gx_lv_px(160)) == 4);
        assert(gx_lv_px(40) / (int32_t)vertical_ticks(gx_lv_px(40)) >=
               lv_font_get_line_height(ui_font(UI_SECONDARY)) + gx_lv_px(8));
        gx_lv_size(p->root, 760, 500);
        for (i = 0; i < 6; i++) {
            ui_plot_mode(p, i % 2);
            settled(p);
            assert(p->density == scale);
            coverage(p);
            ui_plot_zoom(p, 0.8);
            ui_plot_pan(p, 0.02, -0.02);
        }
        ui_plot_fit(p);
        settled(p);
        assert(coverage(p));
        /* A bounded-cache refusal must leave the generic LVGL path usable.
         * Restore the normal dimensions and force one fallback refresh, then
         * request a fresh cache after the simulated unavailable mask. */
        {
            lv_image_header_t header = p->curve.header;
            assert(!curve_prepare(p, 8193, 1));
            assert(!p->mask_ready);
            p->curve.header = header;
            lv_obj_invalidate(p->root);
            settled(p);
            assert(!p->mask_ready && p->painted_count);
            ui_plot_refresh(p);
            settled(p);
            coverage(p);
        }
        gx_lv_size(p->root, 120, 60);
        settled(p);
        gx_lv_size(p->root, 1470, 900);
        settled(p);
        coverage(p);
        ui_plot_bind(p, NULL);
        settled(p);
        assert(!p->count && ui_plot_selected(p) == PLOT_NO_ROW);
        ui_plot_bind(p, m);
    }
    domain_primitives(p);
    ui_plot_bind(p, NULL);
    plot_model_delete(m);
    plot_model_delete(empty);
    lv_obj_delete(p->root);
    gx_lv_close();
    puts("PLOT_COMPONENT passed: cached bounded primitives/ticks/A8 coverage and text runs; AA and fallback; "
         "source-row selection; pointer pan; keyboard/fit/zoom; empty/error state; "
         "1x/2x; robust projection parity for normal/subnormal/extreme domains; resizing and ownership");
    return 0;
}
