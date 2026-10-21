/* SPDX-License-Identifier: MIT */
#include "ui-plot.h"
#include "plot-raster.h"
#include <gx.h>
#include "src/misc/lv_area_private.h"
#include "src/misc/lv_text_private.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <float.h>

#define PLOT_TEXT_RUNS 12
#define PLOT_TEXT_LIMIT (256UL * 1024UL)
struct plot_text {
    lv_image_dsc_t image;
    uint8_t *mask;
    const lv_font_t *font;
    char text[256];
    int32_t advance, left, right, top, bottom;
    unsigned supported, tried;
};
struct ui_plot {
    lv_obj_t *root;
    const plot_model *model;
    struct plot_view view, pressed_view;
    struct plot_vertex *vertices;
    struct gx_a8_primitive *painted;
    lv_image_dsc_t curve;
    uint8_t *mask;
    uint32_t mask_capacity;
    unsigned mask_dirty, mask_ready;
    struct plot_stats stats;
    uint32_t count, capacity, painted_count, selected;
    unsigned mode, dirty, density, ready, dragging;
    unsigned xticks, yticks;
    char xnumbers[5][48], ynumbers[5][48];
    struct plot_text labels[PLOT_TEXT_RUNS];
    uint32_t text_cells;
    int32_t cached_width, cached_height, press_x, press_y;
    char *xlabel, *ylabel, *message;
    ui_plot_select_fn select;
    void *user;
};
static char *copy_text(const char *s) {
    char *out;
    if (!s)
        s = "";
    out = malloc(strlen(s) + 1);
    if (out)
        strcpy(out, s);
    return out;
}
static void replace_text(char **at, const char *text) {
    char *copy = copy_text(text);
    if (copy) {
        free(*at);
        *at = copy;
    }
}
static int plot_area(ui_plot *p, lv_area_t *all, lv_area_t *area) {
    lv_obj_get_content_coords(p->root, all);
    *area = *all;
    area->x1 += gx_lv_px(70);
    area->x2 -= gx_lv_px(16);
    area->y1 += gx_lv_px(30);
    area->y2 -= gx_lv_px(48);
    return area->x2 - area->x1 > gx_lv_px(40) &&
           area->y2 - area->y1 > gx_lv_px(30);
}
static int inside(const struct plot_view *v, double x, double y) {
    return x >= v->xmin && x <= v->xmax && y >= v->ymin && y <= v->ymax;
}
static unsigned vertical_ticks(int32_t height);
static void prepare(ui_plot *p, const lv_area_t *area) {
    int32_t width = area->x2 - area->x1, height = area->y2 - area->y1;
    unsigned logical = width / gx_lv_density();
    uint32_t capacity = (uint32_t)logical * 4 + 64, i;
    double xmin, xmax, ymin, ymax, xspan, yspan, xscale = 0.0, yscale = 0.0;
    int affine, previous_inside = 0;
    int32_t previous_x = 0, previous_y = 0;
    if (!p->dirty && width == p->cached_width && height == p->cached_height &&
        p->density == gx_lv_density())
        return;
    p->dirty = 0;
    p->cached_width = width;
    p->cached_height = height;
    p->density = gx_lv_density();
    p->count = p->painted_count = 0;
    p->mask_dirty = 1;
    p->xticks = width < gx_lv_px(360) ? 2 : 4;
    p->yticks = vertical_ticks(height);
    for (i = 0; i <= p->xticks; i++)
        snprintf(p->xnumbers[i], sizeof(p->xnumbers[i]), "%.4g",
                 plot_axis_value(p->view.xmin, p->view.xmax, (double)i / p->xticks));
    for (i = 0; i <= p->yticks; i++)
        snprintf(p->ynumbers[i], sizeof(p->ynumbers[i]), "%.4g",
                 plot_axis_value(p->view.ymin, p->view.ymax, (double)i / p->yticks));
    memset(&p->stats, 0, sizeof(p->stats));
    if (!p->ready || !p->model)
        return;
    if (capacity > p->capacity) {
        struct plot_vertex *vertices = malloc(capacity * sizeof(*vertices));
        struct gx_a8_primitive *painted = malloc(capacity * sizeof(*painted));
        if (!vertices || !painted) {
            free(vertices);
            free(painted);
            replace_text(&p->message, "Not enough memory to draw the plot.");
            return;
        }
        free(p->vertices);
        free(p->painted);
        p->vertices = vertices;
        p->painted = painted;
        p->capacity = capacity;
    }
    p->count = plot_model_geometry(p->model, &p->view, logical, p->vertices,
                                   p->capacity, &p->stats);
    xmin = p->view.xmin;
    xmax = p->view.xmax;
    ymin = p->view.ymin;
    ymax = p->view.ymax;
    xspan = xmax - xmin;
    yspan = ymax - ymin;
    affine = xspan >= DBL_MIN && xspan <= DBL_MAX && yspan >= DBL_MIN && yspan <= DBL_MAX;
    if (affine) {
        xscale = width / xspan;
        yscale = height / yspan;
        affine = xscale <= DBL_MAX && yscale <= DBL_MAX;
    }
    for (i = 0; i < p->count; i++) {
        struct plot_vertex *v = &p->vertices[i];
        struct gx_a8_primitive *out = &p->painted[p->painted_count];
        double x0, y0, x1, y1;
        int contained = v->x >= xmin && v->x <= xmax && v->y >= ymin && v->y <= ymax;
        int32_t px = 0, py = 0;
        out->size = gx_lv_px(1);
        if (contained && affine) {
            px = (int32_t)((v->x - xmin) * xscale + 0.5);
            py = (int32_t)((ymax - v->y) * yscale + 0.5);
        }
        if (p->mode == PLOT_LINE && i && v->connect && contained && previous_inside && affine) {
            out->x0 = previous_x;
            out->y0 = previous_y;
            out->x1 = px;
            out->y1 = py;
            out->dot = 0;
            p->painted_count++;
        } else if (p->mode == PLOT_LINE && i && v->connect &&
            plot_clip_segment(&p->view, p->vertices[i - 1].x, p->vertices[i - 1].y,
                               v->x, v->y, &x0, &y0, &x1, &y1)) {
            out->x0 = (int32_t)(x0 * width + 0.5);
            out->y0 = (int32_t)((1 - y0) * height + 0.5);
            out->x1 = (int32_t)(x1 * width + 0.5);
            out->y1 = (int32_t)((1 - y1) * height + 0.5);
            out->dot = 0;
            p->painted_count++;
        } else if ((p->mode == PLOT_SCATTER || !v->connect) && contained) {
            out->x0 = affine ? px : (int32_t)(plot_axis_fraction(v->x, xmin, xmax) * width + 0.5);
            out->y0 = affine ? py : (int32_t)((1 - plot_axis_fraction(v->y, ymin, ymax)) * height + 0.5);
            out->x1 = out->x0;
            out->y1 = out->y0;
            out->dot = 1;
            out->size = gx_lv_px(2);
            p->painted_count++;
        }
        previous_inside = contained;
        previous_x = px;
        previous_y = py;
    }
}
/* One coverage image replaces a raster/graphics transition for every scanline
 * of every diagonal. Its lifetime spans all queued strips of the current
 * refresh; input and refresh run serially in the port. No document-sized cache
 * is needed, and allocation failure retains the ordinary LVGL drawing path.
 * C16 A8 samples occupy two physical bytes, so cap this at sixteen MiB. */
#define CURVE_MASK_LIMIT (8UL * 1024UL * 1024UL)
static int curve_prepare(ui_plot *p, uint32_t width, uint32_t height) {
    uint32_t count = width * height, i;
    if (!p->mask_dirty && p->curve.header.w == width && p->curve.header.h == height)
        return p->mask_ready;
    p->mask_dirty = p->mask_ready = 0;
    if (!width || !height || width > 8192 || height > 8192 || count > CURVE_MASK_LIMIT)
        return 0;
    if (p->mask_capacity < count) {
        gx_lv_release_mask(p->mask);
        free(p->mask);
        p->mask = NULL;
        p->mask_capacity = 0;
        p->mask = malloc(count * sizeof(*p->mask));
        if (!p->mask)
            return 0;
        p->mask_capacity = count;
    }
    if (!gx_lv_raster_a8(p->mask, width, height, width, p->painted, p->painted_count)) {
        memset(p->mask, 0, count * sizeof(*p->mask));
        for (i = 0; i < p->painted_count; i++) {
            const struct gx_a8_primitive *v = &p->painted[i];
            if (v->dot)
                plot_raster_dot(p->mask, width, height, width, v->x0, v->y0, v->size);
            else
                plot_raster_line(p->mask, width, height, width, v->x0, v->y0, v->x1, v->y1, v->size);
        }
    }
    memset(&p->curve, 0, sizeof(p->curve));
    p->curve.header.magic = LV_IMAGE_HEADER_MAGIC;
    p->curve.header.cf = LV_COLOR_FORMAT_A8;
    p->curve.header.w = width;
    p->curve.header.h = height;
    p->curve.header.stride = width;
    p->curve.data_size = count;
    p->curve.data = p->mask;
    p->mask_ready = 1;
    return 1;
}
static void label(lv_layer_t *layer, lv_obj_t *owner, const char *s, const lv_area_t *area,
                   unsigned role, lv_text_align_t align) {
    lv_draw_label_dsc_t text;
    lv_area_t intersection;
    if (!lv_area_intersect(&intersection, &layer->_clip_area, area))
        return;
    lv_draw_label_dsc_init(&text);
    text.base.obj = owner;
    text.font = ui_font(role);
    text.color = ui_colour(role == UI_ERROR ? UI_TEXT : UI_MUTED);
    text.align = align;
    text.text = s ? s : "";
    lv_draw_label(layer, &text, area);
}
static void text_release(ui_plot *p, struct plot_text *b) {
    if (b->mask) {
        p->text_cells -= b->image.data_size;
        free(b->mask);
    }
    memset(b, 0, sizeof(*b));
}
/* Cache only a single line backed by the port's static A8 fonts. Retain the
 * ordinary label renderer for wrapping, unsupported glyph formats and ink
 * overlaps, whose repeated blending has different rounding from a merged run.
 * A slot is never reused for another label inside a refresh: every queued
 * strip retains its mask until drawing has completed. */
static int text_prepare(ui_plot *p, struct plot_text *b, const char *s,
                        const lv_font_t *font, int32_t available) {
    uint32_t at, count;
    unsigned length = 0, ink = 0;
    int32_t pen, width, height, previous_right;
    uint8_t *mask;
    while (s[length] && length + 1 < sizeof(b->text)) {
        if ((unsigned char)s[length] < 32)
            return 0;
        length++;
    }
    if (s[length])
        return 0;
    if (b->font != font || strcmp(b->text, s)) {
        text_release(p, b);
        b->font = font;
        strcpy(b->text, s);
        b->left = b->top = 0;
        b->right = b->bottom = -1;
        at = 0;
        pen = 0;
        b->supported = 1;
        while (s[at]) {
            lv_font_glyph_dsc_t g;
            uint32_t c = lv_text_encoded_next(s, &at), peek = at;
            uint32_t next = lv_text_encoded_next(s, &peek);
            if (lv_text_is_marker(c))
                continue;
            if (!lv_font_get_glyph_dsc(font, &g, c, next) || g.is_placeholder ||
                !g.resolved_font || !g.resolved_font->static_bitmap ||
                g.resolved_font->subpx != LV_FONT_SUBPX_NONE ||
                (g.box_w && g.box_h && g.format != LV_FONT_GLYPH_FORMAT_A8)) {
                b->supported = 0;
                break;
            }
            if (g.box_w && g.box_h) {
                int32_t left = pen + g.ofs_x, right = left + g.box_w - 1;
                int32_t top = (int32_t)font->line_height - font->base_line - g.box_h - g.ofs_y;
                int32_t bottom = top + g.box_h - 1;
                if (!ink || left < b->left) b->left = left;
                if (!ink || right > b->right) b->right = right;
                if (!ink || top < b->top) b->top = top;
                if (!ink || bottom > b->bottom) b->bottom = bottom;
                ink = 1;
            }
            pen += g.adv_w;
        }
        b->advance = pen;
    }
    if (!b->supported || b->advance > available)
        return 0;
    if (b->mask)
        return 1;
    if (b->tried)
        return 0;
    b->tried = 1;
    width = b->right - b->left + 1;
    height = b->bottom - b->top + 1;
    if (width <= 0 || height <= 0 || width > 4096 || height > 128)
        return 0;
    count = (uint32_t)width * height;
    if (count > PLOT_TEXT_LIMIT || p->text_cells > PLOT_TEXT_LIMIT - count)
        return 0;
    mask = calloc(count, sizeof(*mask));
    if (!mask)
        return 0;
    at = 0;
    pen = 0;
    previous_right = -32768;
    while (s[at]) {
        lv_font_glyph_dsc_t g;
        const uint8_t *pixels;
        uint32_t c = lv_text_encoded_next(s, &at), peek = at;
        uint32_t next = lv_text_encoded_next(s, &peek);
        int32_t x, y, row, col, stride;
        if (lv_text_is_marker(c))
            continue;
        if (!lv_font_get_glyph_dsc(font, &g, c, next))
            goto fallback;
        if (!g.box_w || !g.box_h) {
            pen += g.adv_w;
            continue;
        }
        pixels = lv_font_get_glyph_static_bitmap(&g);
        if (!pixels)
            goto fallback;
        x = pen + g.ofs_x - b->left;
        y = (int32_t)font->line_height - font->base_line - g.box_h - g.ofs_y - b->top;
        stride = g.stride ? g.stride : g.box_w;
        if (x < 0 || y < 0 || x + g.box_w > width || y + g.box_h > height) {
            lv_font_glyph_release_draw_data(&g);
            goto fallback;
        }
        for (row = 0; row < g.box_h; row++) {
            uint8_t *dst = mask + (uint32_t)(y + row) * width + x;
            const uint8_t *src = pixels + (uint32_t)row * stride;
            if (x > previous_right)
                memcpy(dst, src, g.box_w);
            else for (col = 0; col < g.box_w; col++) {
                if (!src[col])
                    continue;
                if (dst[col]) {
                    lv_font_glyph_release_draw_data(&g);
                    goto fallback;
                }
                dst[col] = src[col];
            }
        }
        if (x + g.box_w - 1 > previous_right)
            previous_right = x + g.box_w - 1;
        pen += g.adv_w;
        lv_font_glyph_release_draw_data(&g);
    }
    b->mask = mask;
    b->image.header.magic = LV_IMAGE_HEADER_MAGIC;
    b->image.header.cf = LV_COLOR_FORMAT_A8;
    b->image.header.w = width;
    b->image.header.h = height;
    b->image.header.stride = width;
    b->image.data_size = count;
    b->image.data = mask;
    p->text_cells += count;
    return 1;
fallback:
    free(mask);
    return 0;
}
static void cached_label(ui_plot *p, unsigned slot, lv_layer_t *layer,
                          const char *s, const lv_area_t *area,
                          unsigned role, lv_text_align_t align) {
    lv_area_t intersection;
    struct plot_text *b = &p->labels[slot];
    int32_t width = lv_area_get_width(area), offset = 0;
    if (!s || !*s || !lv_area_intersect(&intersection, &layer->_clip_area, area))
        return;
    if (text_prepare(p, b, s, ui_font(role), width)) {
        lv_draw_image_dsc_t image;
        lv_area_t coords;
        if (align == LV_TEXT_ALIGN_CENTER)
            offset = (width - b->advance) / 2;
        else if (align == LV_TEXT_ALIGN_RIGHT)
            offset = width - b->advance;
        coords.x1 = area->x1 + offset + b->left;
        coords.y1 = area->y1 + b->top;
        coords.x2 = coords.x1 + b->image.header.w - 1;
        coords.y2 = coords.y1 + b->image.header.h - 1;
        lv_draw_image_dsc_init(&image);
        image.base.obj = p->root;
        image.src = &b->image;
        image.recolor = ui_colour(role == UI_ERROR ? UI_TEXT : UI_MUTED);
        lv_draw_image(layer, &image, &coords);
    } else
        label(layer, p->root, s, area, role, align);
}
static void line(lv_layer_t *layer, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                  unsigned colour, unsigned width) {
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = x0;
    d.p1.y = y0;
    d.p2.x = x1;
    d.p2.y = y1;
    d.color = ui_colour(colour);
    d.width = gx_lv_px(width);
    lv_draw_line(layer, &d);
}
static void point(lv_layer_t *layer, int32_t x, int32_t y, unsigned radius, unsigned colour) {
    lv_draw_rect_dsc_t dot;
    lv_area_t area;
    int32_t r = gx_lv_px(radius);
    lv_draw_rect_dsc_init(&dot);
    dot.bg_opa = LV_OPA_COVER;
    dot.bg_color = ui_colour(colour);
    dot.radius = LV_RADIUS_CIRCLE;
    area.x1 = x - r;
    area.y1 = y - r;
    area.x2 = x + r;
    area.y2 = y + r;
    lv_draw_rect(layer, &dot, &area);
}
static unsigned vertical_ticks(int32_t height) {
    return height < gx_lv_px(72) ? 1 : height < gx_lv_px(144) ? 2 : 4;
}
static void paint(ui_plot *p, lv_layer_t *layer) {
    lv_area_t all, area, text, saved = layer->_clip_area, clip;
    unsigned ticks, i;
    int32_t width, height, fh;
    if (!plot_area(p, &all, &area) || !p->ready) {
        lv_obj_get_content_coords(p->root, &all);
        text = all;
        text.x1 += gx_lv_px(12);
        text.x2 -= gx_lv_px(12);
        text.y1 += (all.y2 - all.y1 - lv_font_get_line_height(ui_font(UI_BODY))) / 2;
        label(layer, p->root, p->message && *p->message ? p->message : "Choose numeric columns to plot.",
              &text, UI_BODY, LV_TEXT_ALIGN_CENTER);
        return;
    }
    prepare(p, &area);
    if (!lv_area_intersect(&clip, &saved, &all))
        return;
    layer->_clip_area = clip;
    width = area.x2 - area.x1;
    height = area.y2 - area.y1;
    fh = lv_font_get_line_height(ui_font(UI_SECONDARY));
    ticks = p->xticks;
    for (i = 0; i <= ticks; i++) {
        double t = (double)i / ticks;
        int32_t x = area.x1 + (int32_t)(t * width);
        line(layer, x, area.y1, x, area.y2, UI_BORDER, 1);
        text.x1 = x - gx_lv_px(35);
        text.x2 = x + gx_lv_px(35);
        text.y1 = area.y2 + gx_lv_px(8);
        text.y2 = text.y1 + fh;
        cached_label(p, i, layer, p->xnumbers[i], &text, UI_SECONDARY, LV_TEXT_ALIGN_CENTER);
    }
    ticks = p->yticks;
    for (i = 0; i <= ticks; i++) {
        double t = (double)i / ticks;
        int32_t y = area.y2 - (int32_t)(t * height);
        line(layer, area.x1, y, area.x2, y, UI_BORDER, 1);
        text.x1 = all.x1 + gx_lv_px(2);
        text.x2 = area.x1 - gx_lv_px(9);
        text.y1 = y - fh / 2;
        text.y2 = text.y1 + fh;
        cached_label(p, 5 + i, layer, p->ynumbers[i], &text, UI_SECONDARY, LV_TEXT_ALIGN_RIGHT);
    }
    text.x1 = area.x1;
    text.x2 = area.x2;
    text.y1 = area.y2 + gx_lv_px(28);
    text.y2 = all.y2;
    cached_label(p, 10, layer, p->xlabel, &text, UI_SECONDARY, LV_TEXT_ALIGN_CENTER);
    text.y1 = all.y1 + gx_lv_px(5);
    text.y2 = area.y1 - gx_lv_px(4);
    cached_label(p, 11, layer, p->ylabel, &text, UI_SECONDARY, LV_TEXT_ALIGN_LEFT);
    if (lv_area_intersect(&clip, &saved, &area)) {
        layer->_clip_area = clip;
        if (curve_prepare(p, (uint32_t)width + 1, (uint32_t)height + 1)) {
            lv_draw_image_dsc_t image;
            lv_draw_image_dsc_init(&image);
            image.base.obj = p->root;
            image.src = &p->curve;
            image.recolor = ui_colour(UI_ACCENT);
            lv_draw_image(layer, &image, &area);
        } else for (i = 0; i < p->painted_count; i++) {
            struct gx_a8_primitive *v = &p->painted[i];
            int32_t x0 = area.x1 + v->x0, y0 = area.y1 + v->y0;
            int32_t x1 = area.x1 + v->x1, y1 = area.y1 + v->y1;
            int32_t left = x0 < x1 ? x0 : x1, right = x0 > x1 ? x0 : x1;
            int32_t top = y0 < y1 ? y0 : y1, bottom = y0 > y1 ? y0 : y1;
            if (right + gx_lv_px(2) < clip.x1 || left - gx_lv_px(2) > clip.x2 ||
                bottom + gx_lv_px(2) < clip.y1 || top - gx_lv_px(2) > clip.y2)
                continue;
            if (v->dot)
                point(layer, x0, y0, 2, UI_ACCENT);
            else
                line(layer, x0, y0, x1, y1, UI_ACCENT, 1);
        }
        {
            const struct plot_point *s = plot_model_point(p->model, p->selected);
            if (s && s->valid && inside(&p->view, s->x, s->y)) {
                int32_t x = area.x1 + (int32_t)(plot_axis_fraction(s->x, p->view.xmin, p->view.xmax) * width);
                int32_t y = area.y2 - (int32_t)(plot_axis_fraction(s->y, p->view.ymin, p->view.ymax) * height);
                point(layer, x, y, 4, UI_TEXT);
                point(layer, x, y, 2, UI_ACCENT);
            }
        }
    }
    layer->_clip_area = saved;
    if (!p->count && p->message && *p->message) {
        text = area;
        text.y1 += height / 2;
        label(layer, p->root, p->message, &text, UI_BODY, LV_TEXT_ALIGN_CENTER);
    }
}
static void choose(ui_plot *p, int32_t x, int32_t y) {
    lv_area_t all, area;
    uint32_t i, best = PLOT_NO_ROW;
    double nearest = (double)gx_lv_px(10) * gx_lv_px(10);
    if (!p->ready || !plot_area(p, &all, &area))
        return;
    prepare(p, &area);
    for (i = 0; i < p->count; i++) {
        struct plot_vertex *v = &p->vertices[i];
        double dx, dy, distance;
        if (!inside(&p->view, v->x, v->y))
            continue;
        dx = area.x1 + plot_axis_fraction(v->x, p->view.xmin, p->view.xmax) *
             (area.x2 - area.x1) - x;
        dy = area.y2 - plot_axis_fraction(v->y, p->view.ymin, p->view.ymax) *
             (area.y2 - area.y1) - y;
        distance = dx * dx + dy * dy;
        if (distance <= nearest) {
            nearest = distance;
            best = v->index;
        }
    }
    ui_plot_select(p, best);
    if (best != PLOT_NO_ROW && p->select)
        p->select(p->user, plot_model_point(p->model, best)->row);
}
static void plot_event(lv_event_t *event) {
    ui_plot *p = lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_DRAW_MAIN)
        paint(p, lv_event_get_layer(event));
    else if (code == LV_EVENT_DELETE) {
        unsigned i;
        for (i = 0; i < PLOT_TEXT_RUNS; i++)
            text_release(p, &p->labels[i]);
        free(p->vertices);
        free(p->painted);
        gx_lv_release_mask(p->mask);
        free(p->mask);
        free(p->xlabel);
        free(p->ylabel);
        free(p->message);
        free(p);
    } else if (code == LV_EVENT_SIZE_CHANGED)
        p->dirty = 1;
    else if (lv_obj_has_state(p->root, LV_STATE_DISABLED))
        p->dragging = 0;
    else if (code == LV_EVENT_DOUBLE_CLICKED)
        ui_plot_fit(p);
    else if (code == LV_EVENT_PRESSED && lv_indev_active()) {
        lv_point_t point;
        lv_indev_get_point(lv_indev_active(), &point);
        p->press_x = point.x;
        p->press_y = point.y;
        p->pressed_view = p->view;
        p->dragging = 0;
    } else if (code == LV_EVENT_PRESSING && lv_indev_active() && p->ready) {
        lv_point_t point;
        lv_area_t all, area;
        int32_t dx, dy;
        lv_indev_get_point(lv_indev_active(), &point);
        dx = point.x - p->press_x;
        dy = point.y - p->press_y;
        if (dx > gx_lv_px(3) || dx < -gx_lv_px(3) ||
            dy > gx_lv_px(3) || dy < -gx_lv_px(3))
            p->dragging = 1;
        if (p->dragging && plot_area(p, &all, &area)) {
            struct plot_view moved = p->pressed_view;
            if (plot_view_pan(&moved, -(double)dx / (area.x2 - area.x1),
                               (double)dy / (area.y2 - area.y1)))
                ui_plot_set_view(p, &moved);
        }
    } else if (code == LV_EVENT_RELEASED && lv_indev_active()) {
        lv_point_t point;
        lv_indev_get_point(lv_indev_active(), &point);
        if (!p->dragging)
            choose(p, point.x, point.y);
    } else if (code == LV_EVENT_KEY) {
        uint32_t key = lv_event_get_key(event);
        if (key == LV_KEY_LEFT)
            ui_plot_pan(p, -0.1, 0);
        else if (key == LV_KEY_RIGHT)
            ui_plot_pan(p, 0.1, 0);
        else if (key == LV_KEY_UP)
            ui_plot_pan(p, 0, 0.1);
        else if (key == LV_KEY_DOWN)
            ui_plot_pan(p, 0, -0.1);
        else if (key == '+' || key == '=')
            ui_plot_zoom(p, 0.8);
        else if (key == '-' || key == '_')
            ui_plot_zoom(p, 1.25);
        else if (key == LV_KEY_HOME || key == '0')
            ui_plot_fit(p);
    }
}
ui_plot *ui_plot_create(lv_obj_t *parent) {
    ui_plot *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->selected = PLOT_NO_ROW;
    p->dirty = 1;
    p->root = ui_panel(parent);
    lv_obj_set_layout(p->root, LV_LAYOUT_NONE);
    lv_obj_set_scrollable(p->root, false);
    lv_obj_set_clickable(p->root, true);
    lv_obj_set_style_pad_all(p->root, 0, 0);
    lv_obj_set_style_border_width(p->root, gx_lv_px(1), 0);
    lv_obj_set_style_border_color(p->root, ui_colour(UI_BORDER), 0);
    lv_obj_set_style_border_color(p->root, ui_colour(UI_ACCENT), LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(p->root, ui_colour(UI_CANVAS), 0);
    lv_obj_set_width(p->root, lv_pct(100));
    lv_obj_set_height(p->root, gx_lv_px(260));
    lv_obj_add_event_cb(p->root, plot_event, LV_EVENT_ALL, p);
    lv_group_add_obj(gx_lv_group(), p->root);
    return p;
}
lv_obj_t *ui_plot_object(ui_plot *p) { return p ? p->root : NULL; }
void ui_plot_bind(ui_plot *p, const plot_model *m) {
    if (!p)
        return;
    p->model = m;
    p->selected = PLOT_NO_ROW;
    p->ready = plot_model_fit(m, &p->view);
    p->count = p->painted_count = 0;
    memset(&p->stats, 0, sizeof(p->stats));
    replace_text(&p->message, m && plot_model_ready(m) ?
                  "No valid numeric samples." : "Choose numeric columns to plot.");
    ui_plot_refresh(p);
}
void ui_plot_labels(ui_plot *p, const char *x, const char *y) {
    if (p) {
        replace_text(&p->xlabel, x);
        replace_text(&p->ylabel, y);
        lv_obj_invalidate(p->root);
    }
}
void ui_plot_mode(ui_plot *p, unsigned mode) {
    if (p && mode <= PLOT_SCATTER && p->mode != mode) {
        p->mode = mode;
        ui_plot_refresh(p);
    }
}
void ui_plot_message(ui_plot *p, const char *message) {
    if (p) {
        replace_text(&p->message, message);
        lv_obj_invalidate(p->root);
    }
}
void ui_plot_fit(ui_plot *p) {
    if (p) {
        p->ready = plot_model_fit(p->model, &p->view);
        ui_plot_refresh(p);
    }
}
int ui_plot_get_view(ui_plot *p, struct plot_view *view) {
    if (!p || !p->ready || !view)
        return 0;
    *view = p->view;
    return 1;
}
int ui_plot_set_view(ui_plot *p, const struct plot_view *view) {
    if (!p || !p->ready || !plot_view_valid(view))
        return 0;
    if (p->view.xmin == view->xmin && p->view.xmax == view->xmax &&
        p->view.ymin == view->ymin && p->view.ymax == view->ymax)
        return 1;
    p->view = *view;
    ui_plot_refresh(p);
    return 1;
}
void ui_plot_pan(ui_plot *p, double x, double y) {
    struct plot_view v;
    if (ui_plot_get_view(p, &v) && plot_view_pan(&v, x, y))
        ui_plot_set_view(p, &v);
}
void ui_plot_zoom(ui_plot *p, double factor) {
    struct plot_view v;
    if (ui_plot_get_view(p, &v) && plot_view_zoom(&v, factor, 0.5, 0.5))
        ui_plot_set_view(p, &v);
}
void ui_plot_refresh(ui_plot *p) {
    if (p) {
        p->dirty = 1;
        lv_obj_invalidate(p->root);
    }
}
void ui_plot_density(ui_plot *p) {
    if (p) {
        lv_obj_set_style_border_width(p->root, gx_lv_px(1), 0);
        ui_plot_refresh(p);
    }
}
void ui_plot_focus(ui_plot *p) {
    if (p)
        lv_group_focus_obj(p->root);
}
int ui_plot_focused(ui_plot *p) {
    return p && lv_group_get_focused(gx_lv_group()) == p->root;
}
void ui_plot_callback(ui_plot *p, ui_plot_select_fn select, void *user) {
    if (p) {
        p->select = select;
        p->user = user;
    }
}
void ui_plot_select(ui_plot *p, uint32_t index) {
    const struct plot_point *point;
    if (!p)
        return;
    point = plot_model_point(p->model, index);
    p->selected = point && point->valid ? index : PLOT_NO_ROW;
    lv_obj_invalidate(p->root);
}
uint32_t ui_plot_selected(ui_plot *p) {
    const struct plot_point *point = p ? plot_model_point(p->model, p->selected) : NULL;
    return point && point->valid ? point->row : PLOT_NO_ROW;
}
void ui_plot_stats(ui_plot *p, struct plot_stats *stats) {
    if (stats) {
        if (p)
            *stats = p->stats;
        else
            memset(stats, 0, sizeof(*stats));
    }
}
