/* SPDX-License-Identifier: MIT */
#include "ui-data.h"
/* Pinned LVGL text metrics/UTF-8 helpers; draw clipping stays in this module. */
#include "src/misc/lv_text_private.h"
#include "src/misc/lv_area_private.h"
#include <gx.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Text is cached in a bounded viewport ring. Only headers are widgets. */
#define NUMBER_WIDTH 56
#define POOL_ROWS ((GX_HOSTED_MAX_HEIGHT + UI_ROW_HEIGHT - 1) / UI_ROW_HEIGHT + 1)
#define POOL_COLS ((GX_HOSTED_MAX_WIDTH - NUMBER_WIDTH + 63) / 64 + 2)
/* Keep memory bounded independently of document size. C16 A8 samples use one
 * logical C character (two host bytes), so this caps masks at 4 MiB per table. */
#define TEXT_MASK_LIMIT (2UL * 1024UL * 1024UL)
struct text_bitmap {
    lv_image_dsc_t image;
    uint8_t *mask;
    int32_t advance, left, right, top, bottom;
    unsigned tried;
};
struct table_cell {
    char text[256];
    uint32_t row, generation;
    unsigned column, width, scale;
    struct text_bitmap bitmap;
};
struct ui_table {
    lv_obj_t *root, *head, *body, *extent, *corner;
    lv_obj_t *headers[POOL_COLS];
    struct table_cell *cells[POOL_ROWS][POOL_COLS];
    char numbers[POOL_ROWS][16];
    uint32_t number_rows[POOL_ROWS], generation;
    unsigned number_valid[POOL_ROWS];
    unsigned number_scale[POOL_ROWS];
    struct text_bitmap number_bitmaps[POOL_ROWS];
    uint32_t mask_cells, mask_limit;
    int32_t view_x, view_width;
    unsigned view_scale, density, header_dirty, header_start, header_count;
    uint32_t header_revision, header_versions[POOL_COLS];
    unsigned header_columns[POOL_COLS], widths[UI_TABLE_COLUMNS];
    char *names[UI_TABLE_COLUMNS];
    uint32_t rows, row;
    unsigned columns, column, pool_cols, busy, dragging, drag_column, enabled;
    int sorted, descending;
    int32_t drag_x, drag_width;
    ui_table_value_fn value;
    ui_table_select_fn select;
    ui_table_sort_fn sort;
    ui_table_key_fn key;
    void *user;
};
enum {
    TABLE_ROOT,
    TABLE_HEAD,
    TABLE_BODY,
    TABLE_CELL,
    TABLE_NUMBER,
    TABLE_CORNER,
    TABLE_SELECTED,
    TABLE_HEADER,
    TABLE_HEADER_ACTIVE,
    TABLE_HEADER_TEXT,
    RADIO,
    RADIO_CHECKED,
    OVERLAY,
    FRAME,
    MENU_FRAME,
    RULE,
    STYLE_COUNT
};
static lv_style_t styles[STYLE_COUNT];
static int ready;
struct ui_dialog {
    lv_obj_t *backdrop, *frame, *body, *footer, *saved;
    lv_group_t *previous, *group;
    ui_dialog_fn action;
    ui_dialog_key_fn key;
    ui_menu_fn menu_action;
    void *user;
    int menu, replace;
    unsigned width, count, default_action, has_default;
    int32_t anchor_x, anchor_y;
};
static ui_dialog *overlay;

static void preview(char *out, unsigned cap, const char *s) {
    unsigned at = 0;
    while (*s && at + 5 < cap) {
        unsigned c = (unsigned char)*s, n = c < 128 ? 1 : c < 224 ? 2 : c < 240 ? 3 : 4, i;
        if (at + n + 4 >= cap)
            break;
        if (c < 32 || c == 127) {
            out[at++] = ' ';
            s++;
            continue;
        }
        for (i = 0; i < n && *s; i++)
            out[at++] = *s++;
    }
    if (*s) {
        out[at++] = '.';
        out[at++] = '.';
        out[at++] = '.';
    }
    out[at] = 0;
}
static int32_t column_x(ui_table *t, unsigned column) {
    int32_t x = gx_lv_px(NUMBER_WIDTH);
    unsigned i;
    for (i = 0; i < column; i++)
        x += gx_lv_px(t->widths[i]);
    return x;
}
static void table_extent(ui_table *t) {
    lv_obj_set_width(t->extent, column_x(t, t->columns));
    lv_obj_set_height(t->extent, (int32_t)t->rows * gx_lv_px(UI_ROW_HEIGHT));
}
static void header_event(lv_event_t *event) {
    ui_table *t = lv_event_get_user_data(event);
    lv_obj_t *obj = lv_event_get_target_obj(event);
    lv_event_code_t code = lv_event_get_code(event);
    unsigned i, col;
    lv_point_t point;
    lv_area_t area;
    if (!t->enabled)
        return;
    for (i = 0; i < t->pool_cols && t->headers[i] != obj; i++)
        ;
    if (i == t->pool_cols)
        return;
    col = t->header_columns[i];
    if (code == LV_EVENT_PRESSED && lv_indev_active()) {
        lv_indev_get_point(lv_indev_active(), &point);
        lv_obj_get_coords(obj, &area);
        t->dragging = point.x >= area.x2 - gx_lv_px(7);
        t->drag_column = col;
        t->drag_x = point.x;
        t->drag_width = t->widths[col];
    } else if (code == LV_EVENT_PRESSING && t->dragging && lv_indev_active()) {
        int32_t width;
        lv_indev_get_point(lv_indev_active(), &point);
        width = t->drag_width + (point.x - t->drag_x) / (int32_t)gx_lv_density();
        ui_table_column_width(t, t->drag_column, width < 64 ? 64 : width > 480 ? 480 : width);
    } else if (code == LV_EVENT_CLICKED) {
        if (!t->dragging && t->sort)
            t->sort(t->user, col);
        t->dragging = 0;
        if (!ui_overlay_active())
            lv_group_focus_obj(t->body);
    } else if (code == LV_EVENT_PRESS_LOST)
        t->dragging = 0;
}
/* Scroll never changes cell-widget geometry or triggers whole-screen layout. */
static void table_viewport(ui_table *t) {
    int32_t sx, view_w, x;
    unsigned start = 0, i, col, cols, dirty;
    char text[256];
    if (t->busy)
        return;
    t->busy = 1;
    sx = lv_obj_get_scroll_x(t->body);
    if (sx < 0)
        sx = 0;
    view_w = lv_obj_get_content_width(t->body);
    dirty = t->header_dirty || view_w != t->view_width || gx_lv_density() != t->view_scale;
    if (!dirty && sx == t->view_x) {
        t->busy = 0;
        return;
    }
    /* Scroll coordinates translate the native header widgets without changing
     * layout properties for every trackpad pixel. The row-number gutter floats. */
    lv_obj_scroll_by(t->head, lv_obj_get_scroll_x(t->head) - sx, 0, LV_ANIM_OFF);
    t->view_x = sx;
    t->view_width = view_w;
    t->view_scale = gx_lv_density();
    x = gx_lv_px(NUMBER_WIDTH);
    while (start < t->columns && x + gx_lv_px(t->widths[start]) <= sx + gx_lv_px(NUMBER_WIDTH))
        x += gx_lv_px(t->widths[start++]);
    cols = 0;
    {
        int32_t end = x;
        while (start + cols < t->columns && cols < POOL_COLS && end < sx + view_w + gx_lv_px(64)) {
            end += gx_lv_px(t->widths[start + cols]);
            cols++;
        }
    }
    if (!dirty && start == t->header_start && cols == t->header_count) {
        t->busy = 0;
        return;
    }
    if (dirty)
        t->header_revision++;
    t->header_dirty = 0;
    t->header_start = start;
    t->header_count = cols;
    for (col = start; col < start + cols; col++) {
        /* Retain headers which are still visible. Crossing a column edge only
         * rebinds the incoming header, rather than every header in the pool. */
        for (i = 0; i < t->pool_cols && t->header_columns[i] != col; i++)
            ;
        if (i == t->pool_cols) {
            for (i = 0; i < t->pool_cols; i++)
                if (t->header_columns[i] < start || t->header_columns[i] >= start + cols)
                    break;
        } else if (!dirty && t->header_versions[i] == t->header_revision) {
            lv_obj_set_hidden(t->headers[i], false);
            continue;
        }
        if (i == t->pool_cols) {
            t->headers[i] = ui_button(t->head, NULL, UI_TOOL);
            lv_group_remove_obj(t->headers[i]);
            lv_obj_add_style(t->headers[i], &styles[TABLE_HEADER], 0);
            lv_obj_add_style(t->headers[i], &styles[TABLE_HEADER_ACTIVE], LV_STATE_CHECKED);
            lv_obj_add_style(ui_label(t->headers[i], "", UI_SECONDARY), &styles[TABLE_HEADER_TEXT],
                             0);
            lv_obj_set_width(lv_obj_get_child(t->headers[i], 0), lv_pct(100));
            lv_label_set_long_mode(lv_obj_get_child(t->headers[i], 0), LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_press_lock(t->headers[i], true);
            lv_obj_add_event_cb(t->headers[i], header_event, LV_EVENT_ALL, t);
            t->pool_cols++;
        }
        t->header_versions[i] = t->header_revision;
        t->header_columns[i] = col;
        lv_obj_set_hidden(t->headers[i], false);
        if (t->sorted == (int)col)
            lv_obj_add_state(t->headers[i], LV_STATE_CHECKED);
        else
            lv_obj_remove_state(t->headers[i], LV_STATE_CHECKED);
        preview(text, sizeof(text) - 5, t->names[col] ? t->names[col] : "");
        if (!text[0])
            snprintf(text, sizeof(text), "Column %u", col + 1);
        if (t->sorted == (int)col)
            strcat(text, t->descending ? "  v" : "  ^");
        if (strcmp(lv_label_get_text(lv_obj_get_child(t->headers[i], 0)), text))
            lv_label_set_text(lv_obj_get_child(t->headers[i], 0), text);
        lv_obj_set_pos(t->headers[i], column_x(t, col), 0);
        lv_obj_set_width(t->headers[i], gx_lv_px(t->widths[col]));
    }
    for (i = 0; i < t->pool_cols; i++)
        lv_obj_set_hidden(t->headers[i],
                          t->header_columns[i] < start || t->header_columns[i] >= start + cols);
    lv_obj_move_foreground(t->corner);
    t->busy = 0;
}
static void bitmap_free(ui_table *t, struct text_bitmap *b) {
    if (b->mask) {
        t->mask_cells -= b->image.data_size;
        free(b->mask);
    }
    memset(b, 0, sizeof(*b));
}
static void bitmap_release(ui_table *t, struct text_bitmap *b) {
    if (b->mask) {
        t->mask_cells -= b->image.data_size;
        free(b->mask);
    }
    b->mask = NULL;
    b->tried = 0;
    memset(&b->image, 0, sizeof(b->image));
}
/* Only discard offscreen runs. Every currently visible run remains pinned until
 * LVGL has finished its draw tasks, including all partial-buffer strips. */
static int bitmap_room(ui_table *t, uint32_t count) {
    int32_t sy, h;
    uint32_t first, last;
    unsigned i, j;
    if (count > t->mask_limit)
        return 0;
    if (t->mask_cells <= t->mask_limit - count)
        return 1;
    sy = lv_obj_get_scroll_y(t->body);
    h = gx_lv_px(UI_ROW_HEIGHT);
    first = sy > 0 ? (uint32_t)sy / h : 0;
    last = ((sy > 0 ? (uint32_t)sy : 0) + lv_obj_get_content_height(t->body) + h - 1) / h;
    for (i = 0; i < POOL_ROWS; i++) {
        if (t->number_rows[i] < first || t->number_rows[i] >= last)
            bitmap_release(t, &t->number_bitmaps[i]);
        for (j = 0; j < POOL_COLS; j++) {
            struct table_cell *cell = t->cells[i][j];
            if (cell && (cell->row < first || cell->row >= last || cell->column < t->header_start ||
                         cell->column >= t->header_start + t->header_count))
                bitmap_release(t, &cell->bitmap);
        }
    }
    return t->mask_cells <= t->mask_limit - count;
}
static void table_bitmaps_clear(ui_table *t) {
    unsigned i, j;
    for (i = 0; i < POOL_ROWS; i++) {
        bitmap_free(t, &t->number_bitmaps[i]);
        for (j = 0; j < POOL_COLS; j++)
            if (t->cells[i][j])
                bitmap_free(t, &t->cells[i][j]->bitmap);
    }
}
void ui_table_refresh(ui_table *t) {
    table_bitmaps_clear(t);
    memset(t->number_valid, 0, sizeof(t->number_valid));
    t->generation++;
    t->header_dirty = 1;
    table_viewport(t);
    lv_obj_invalidate(t->body);
}
/* Measure ink separately from advance, retaining overhangs and combining marks. */
static void text_bounds(struct text_bitmap *b, const char *text, const lv_font_t *font) {
    uint32_t at = 0;
    int32_t width = 0;
    unsigned ink = 0;
    b->left = b->top = 0;
    b->right = b->bottom = -1;
    while (text[at]) {
        lv_font_glyph_dsc_t glyph;
        uint32_t c = lv_text_encoded_next(text, &at), peek = at;
        uint32_t next = lv_text_encoded_next(text, &peek);
        if (lv_text_is_marker(c))
            continue;
        lv_font_get_glyph_dsc(font, &glyph, c, next);
        if (glyph.box_w && glyph.box_h) {
            int32_t left = width + glyph.ofs_x, right = left + glyph.box_w - 1;
            int32_t top = (int32_t)font->line_height - font->base_line - glyph.box_h - glyph.ofs_y;
            int32_t bottom = top + glyph.box_h - 1;
            if (!ink || left < b->left)
                b->left = left;
            if (!ink || right > b->right)
                b->right = right;
            if (!ink || top < b->top)
                b->top = top;
            if (!ink || bottom > b->bottom)
                b->bottom = bottom;
            ink = 1;
        }
        width += glyph.adv_w;
    }
    b->advance = width;
}
/* Bake one A8 text run from static font glyphs. Preserve LVGL's exact blend
 * rounding: if nonzero glyph pixels overlap, retain the original label path.
 * No live mask is evicted while LVGL may still have a queued draw task for it.
 * Ring replacement, document refresh and destruction release the storage. */
static int bitmap_cache(ui_table *t, struct text_bitmap *b, const char *text,
                        const lv_font_t *font) {
    uint32_t at = 0, count;
    int32_t width = b->right - b->left + 1, height = b->bottom - b->top + 1;
    int32_t pen = 0, previous_right = -32768;
    uint8_t *mask;
    if (b->tried)
        return b->mask != NULL;
    b->tried = 1;
    if (width <= 0 || height <= 0 || width > 4096 || height > 256)
        return 0;
    count = (uint32_t)width * height;
    if (!bitmap_room(t, count))
        return 0;
    mask = calloc(count, sizeof(*mask));
    if (!mask)
        return 0;
    while (text[at]) {
        lv_font_glyph_dsc_t g;
        const uint8_t *pixels;
        uint32_t c = lv_text_encoded_next(text, &at), peek = at;
        uint32_t next = lv_text_encoded_next(text, &peek);
        int32_t x, y, row, col, stride;
        if (lv_text_is_marker(c))
            continue;
        if (!lv_font_get_glyph_dsc(font, &g, c, next))
            goto fallback;
        if (!g.box_w || !g.box_h) {
            pen += g.adv_w;
            continue;
        }
        if (!g.resolved_font || !g.resolved_font->static_bitmap ||
            g.format != LV_FONT_GLYPH_FORMAT_A8 || g.is_placeholder ||
            g.resolved_font->subpx != LV_FONT_SUBPX_NONE)
            goto fallback;
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
            else {
                for (col = 0; col < g.box_w; col++) {
                    if (!src[col])
                        continue;
                    if (dst[col]) {
                        lv_font_glyph_release_draw_data(&g);
                        goto fallback;
                    }
                    dst[col] = src[col];
                }
            }
        }
        if (x + g.box_w - 1 > previous_right)
            previous_right = x + g.box_w - 1;
        pen += g.adv_w;
        lv_font_glyph_release_draw_data(&g);
    }
    b->mask = mask;
    memset(&b->image, 0, sizeof(b->image));
    b->image.header.magic = LV_IMAGE_HEADER_MAGIC;
    b->image.header.cf = LV_COLOR_FORMAT_A8;
    b->image.header.w = width;
    b->image.header.h = height;
    b->image.header.stride = width;
    b->image.data_size = count;
    b->image.data = mask;
    t->mask_cells += count;
    return 1;
fallback:
    free(mask);
    return 0;
}
static void table_draw_text(ui_table *t, struct text_bitmap *b, lv_layer_t *layer,
                            lv_draw_label_dsc_t *text, const lv_area_t *area) {
    if (bitmap_cache(t, b, text->text, text->font)) {
        lv_draw_image_dsc_t image;
        lv_area_t coords;
        lv_draw_image_dsc_init(&image);
        image.base.obj = t->body;
        image.src = &b->image;
        image.recolor = text->color;
        coords.x1 = area->x1 + b->left;
        coords.y1 = area->y1 + b->top;
        coords.x2 = coords.x1 + b->image.header.w - 1;
        coords.y2 = coords.y1 + b->image.header.h - 1;
        lv_draw_image(layer, &image, &coords);
    } else
        lv_draw_label(layer, text, area);
}
/* Cache a single-line preview with UTF-8-safe ellipsis and pixel width. */
static struct table_cell *table_text(ui_table *t, uint32_t row, unsigned col) {
    struct table_cell **slot = &t->cells[row % POOL_ROWS][col % POOL_COLS];
    struct table_cell *cell = *slot;
    unsigned scale = gx_lv_density();
    int32_t limit = gx_lv_px(t->widths[col] - 18);
    if (!cell) {
        cell = calloc(1, sizeof(*cell));
        *slot = cell;
    }
    if (!cell)
        return NULL;
    if (cell->generation != t->generation || cell->row != row || cell->column != col ||
        cell->width != t->widths[col] || cell->scale != scale) {
        const lv_font_t *font = ui_font(UI_BODY);
        uint32_t at = 0, cut = 0;
        int32_t width = 0, dots = 3 * lv_font_get_glyph_width(font, '.', '.');
        preview(cell->text, sizeof(cell->text), t->value ? t->value(t->user, row, col) : "");
        while (cell->text[at]) {
            uint32_t before = at, next, c = lv_text_encoded_next(cell->text, &at);
            uint32_t peek = at;
            next = lv_text_encoded_next(cell->text, &peek);
            width += lv_font_get_glyph_width(font, c, next);
            if (width <= limit - dots)
                cut = at;
            if (width > limit) {
                if (cut > before)
                    cut = before;
                strcpy(cell->text + cut, "...");
                break;
            }
        }
        bitmap_free(t, &cell->bitmap);
        text_bounds(&cell->bitmap, cell->text, font);
        cell->row = row;
        cell->column = col;
        cell->generation = t->generation;
        cell->width = t->widths[col];
        cell->scale = scale;
    }
    return cell;
}
static void table_paint(ui_table *t, lv_layer_t *layer) {
    lv_area_t area, saved = layer->_clip_area, viewport, clip, cell_area, text_area;
    lv_draw_label_dsc_t text;
    lv_draw_rect_dsc_t fill, selected;
    int32_t sx = lv_obj_get_scroll_x(t->body), sy = lv_obj_get_scroll_y(t->body);
    int32_t h = gx_lv_px(UI_ROW_HEIGHT), gutter = gx_lv_px(NUMBER_WIDTH), x, y;
    uint32_t first = sy > 0 ? (uint32_t)sy / h : 0, row;
    unsigned start = 0, i, col;
    lv_obj_get_content_coords(t->body, &area);
    if (!lv_area_intersect(&viewport, &saved, &area))
        return;
    layer->_clip_area = viewport;
    lv_draw_label_dsc_init(&text);
    /* A widget owner lets LVGL use text_size instead of measuring it again. */
    text.base.obj = t->body;
    text.font = ui_font(UI_BODY);
    text.color = ui_colour(UI_TEXT);
    text.align = LV_TEXT_ALIGN_LEFT;
    text.flag = LV_TEXT_FLAG_EXPAND;
    text.text_size.y = lv_font_get_line_height(text.font);
    lv_draw_rect_dsc_init(&fill);
    fill.bg_opa = LV_OPA_COVER;
    fill.bg_color = ui_colour(UI_BORDER);
    lv_draw_rect_dsc_init(&selected);
    selected.bg_opa = LV_OPA_COVER;
    selected.bg_color = lv_color_hex(0x303d52UL);
    selected.border_opa = LV_OPA_COVER;
    selected.border_width = gx_lv_px(1);
    selected.border_color = ui_colour(UI_ACCENT);
    x = column_x(t, 0) - sx + area.x1;
    while (start < t->columns && x + gx_lv_px(t->widths[start]) <= area.x1 + gutter)
        x += gx_lv_px(t->widths[start++]);
    for (row = first, i = 0; row < t->rows && i < POOL_ROWS; row++, i++) {
        y = area.y1 + (int32_t)row * h - sy;
        if (y > viewport.y2)
            break;
        if (y + h <= viewport.y1)
            continue;
        layer->_clip_area = viewport;
        cell_area.x1 = area.x1 + gutter;
        cell_area.x2 = area.x1 + column_x(t, t->columns) - sx - 1;
        cell_area.y1 = y + h - gx_lv_px(1);
        cell_area.y2 = y + h - 1;
        lv_draw_rect(layer, &fill, &cell_area);
        {
            int32_t cell_x = x;
            for (col = start; col < t->columns && col < start + POOL_COLS; col++) {
                struct table_cell *value;
                int32_t w = gx_lv_px(t->widths[col]);
                if (cell_x > viewport.x2)
                    break;
                cell_area.x1 = cell_x;
                cell_area.x2 = cell_x + w - 1;
                cell_area.y1 = y;
                cell_area.y2 = y + h - 1;
                if (!lv_area_intersect(&clip, &viewport, &cell_area)) {
                    cell_x += w;
                    continue;
                }
                if (clip.x1 < area.x1 + gutter)
                    clip.x1 = area.x1 + gutter;
                if (clip.y1 < area.y1)
                    clip.y1 = area.y1;
                if (clip.y2 > area.y2)
                    clip.y2 = area.y2;
                layer->_clip_area = clip;
                if (row == t->row && col == t->column)
                    lv_draw_rect(layer, &selected, &cell_area);
                value = table_text(t, row, col);
                if (value && value->text[0]) {
                    text.text = value->text;
                    text.text_size.x = value->bitmap.advance;
                    text_area = cell_area;
                    text_area.x1 += gx_lv_px(9);
                    text_area.x2 -= gx_lv_px(9);
                    text_area.y1 += gx_lv_px(4);
                    text_area.y2 -= gx_lv_px(4);
                    if (value->bitmap.right >= value->bitmap.left &&
                        text_area.x1 + value->bitmap.right >= clip.x1 &&
                        text_area.x1 + value->bitmap.left <= clip.x2)
                        table_draw_text(t, &value->bitmap, layer, &text, &text_area);
                }
                cell_x += w;
            }
        }
        /* The fixed gutter is drawn separately, never translated with content. */
        clip = viewport;
        if (clip.x1 < area.x1)
            clip.x1 = area.x1;
        if (clip.x2 > area.x1 + gutter - 1)
            clip.x2 = area.x1 + gutter - 1;
        if (clip.y1 < area.y1)
            clip.y1 = area.y1;
        if (clip.y2 > area.y2)
            clip.y2 = area.y2;
        layer->_clip_area = clip;
        col = row % POOL_ROWS;
        if (!t->number_valid[col] || t->number_rows[col] != row ||
            t->number_scale[col] != gx_lv_density()) {
            snprintf(t->numbers[col], sizeof(t->numbers[col]), "%lu", (unsigned long)row + 1);
            t->number_rows[col] = row;
            t->number_valid[col] = 1;
            t->number_scale[col] = gx_lv_density();
            bitmap_free(t, &t->number_bitmaps[col]);
            text_bounds(&t->number_bitmaps[col], t->numbers[col], ui_font(UI_SECONDARY));
        }
        text.text = t->numbers[col];
        text.font = ui_font(UI_SECONDARY);
        text.color = ui_colour(UI_MUTED);
        text.text_size.x = t->number_bitmaps[col].advance;
        text.text_size.y = lv_font_get_line_height(text.font);
        text_area.x2 = area.x1 + gutter - gx_lv_px(6) - 1;
        text_area.x1 = text_area.x2 + 1 - text.text_size.x;
        text_area.y1 = y + gx_lv_px(4);
        text_area.y2 = y + h - gx_lv_px(4) - 1;
        table_draw_text(t, &t->number_bitmaps[col], layer, &text, &text_area);
        text.font = ui_font(UI_BODY);
        text.color = ui_colour(UI_TEXT);
        text.align = LV_TEXT_ALIGN_LEFT;
        text.text_size.y = lv_font_get_line_height(text.font);
        layer->_clip_area = saved;
    }
    layer->_clip_area = saved;
}
void ui_table_select(ui_table *t, uint32_t row, unsigned col, int reveal) {
    int32_t sx, sy, x, y, w, h;
    if (!t->rows || !t->columns)
        return;
    t->row = row < t->rows ? row : t->rows - 1;
    t->column = col < t->columns ? col : t->columns - 1;
    if (reveal) {
        sx = lv_obj_get_scroll_x(t->body);
        sy = lv_obj_get_scroll_y(t->body);
        x = column_x(t, t->column);
        y = (int32_t)t->row * gx_lv_px(UI_ROW_HEIGHT);
        w = lv_obj_get_content_width(t->body);
        h = lv_obj_get_content_height(t->body);
        if (gx_lv_px(t->widths[t->column]) > w - gx_lv_px(NUMBER_WIDTH))
            sx = x - gx_lv_px(NUMBER_WIDTH);
        else {
            if (x < sx + gx_lv_px(NUMBER_WIDTH))
                sx = x - gx_lv_px(NUMBER_WIDTH);
            if (x + gx_lv_px(t->widths[t->column]) > sx + w)
                sx = x + gx_lv_px(t->widths[t->column]) - w;
        }
        if (y < sy)
            sy = y;
        if (y + gx_lv_px(UI_ROW_HEIGHT) > sy + h)
            sy = y + gx_lv_px(UI_ROW_HEIGHT) - h;
        lv_obj_scroll_to(t->body, sx, sy, LV_ANIM_OFF);
    }
    table_viewport(t);
    lv_obj_invalidate(t->body);
    if (t->select)
        t->select(t->user, t->row, t->column);
}
static void body_event(lv_event_t *e) {
    ui_table *t = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DRAW_MAIN)
        table_paint(t, lv_event_get_layer(e));
    else if (code == LV_EVENT_SCROLL || code == LV_EVENT_SIZE_CHANGED)
        table_viewport(t);
    else if (!t->enabled)
        return;
    else if (code == LV_EVENT_PRESSED && lv_indev_active()) {
        lv_point_t p;
        lv_area_t area;
        int32_t x, y;
        unsigned col = 0;
        lv_indev_get_point(lv_indev_active(), &p);
        lv_obj_get_content_coords(t->body, &area);
        x = p.x - area.x1 + lv_obj_get_scroll_x(t->body);
        y = p.y - area.y1 + lv_obj_get_scroll_y(t->body);
        if (y < 0 || (uint32_t)(y / gx_lv_px(UI_ROW_HEIGHT)) >= t->rows)
            return;
        if (p.x - area.x1 < gx_lv_px(NUMBER_WIDTH))
            col = t->column;
        else
            while (col + 1 < t->columns && column_x(t, col + 1) <= x)
                col++;
        ui_table_select(t, y / gx_lv_px(UI_ROW_HEIGHT), col, 0);
    } else if (code == LV_EVENT_KEY) {
        uint32_t key = lv_event_get_key(e), row = t->row;
        unsigned col = t->column;
        if (t->key && t->key(t->user, key))
            return;
        if (key == LV_KEY_UP && row)
            row--;
        else if (key == LV_KEY_DOWN && row + 1 < t->rows)
            row++;
        else if (key == LV_KEY_LEFT && col)
            col--;
        else if (key == LV_KEY_RIGHT && col + 1 < t->columns)
            col++;
        else if (key == LV_KEY_HOME)
            col = 0;
        else if (key == LV_KEY_END && t->columns)
            col = t->columns - 1;
        else
            return;
        ui_table_select(t, row, col, 1);
    }
}
static void table_deleted(lv_event_t *event) {
    ui_table *t = lv_event_get_user_data(event);
    unsigned i, j;
    /* LVGL sends the parent's DELETE before deleting its children. Their
     * ALL-event policies must stop borrowing this state before it is freed. */
    lv_obj_remove_event_cb_with_user_data(t->body, body_event, t);
    for (i = 0; i < t->pool_cols; i++)
        lv_obj_remove_event_cb_with_user_data(t->headers[i], header_event, t);
    table_bitmaps_clear(t);
    for (i = 0; i < UI_TABLE_COLUMNS; i++)
        free(t->names[i]);
    for (i = 0; i < POOL_ROWS; i++)
        for (j = 0; j < POOL_COLS; j++)
            free(t->cells[i][j]);
    free(t);
}
ui_table *ui_table_create(lv_obj_t *parent, ui_table_value_fn value, void *user) {
    ui_table *t = calloc(1, sizeof(*t));
    unsigned i;
    if (!t)
        return NULL;
    t->mask_limit = TEXT_MASK_LIMIT;
    t->enabled = 1;
    t->density = gx_lv_density();
    t->value = value;
    t->user = user;
    t->sorted = -1;
    t->generation = t->header_dirty = 1;
    for (i = 0; i < UI_TABLE_COLUMNS; i++)
        t->widths[i] = 144;
    t->root = ui_stack(parent, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_add_style(t->root, &styles[TABLE_ROOT], 0);
    lv_obj_add_event_cb(t->root, table_deleted, LV_EVENT_DELETE, t);
    t->head = ui_panel(t->root);
    lv_obj_add_style(t->head, &styles[TABLE_HEAD], 0);
    lv_obj_set_width(t->head, lv_pct(100));
    lv_obj_set_scrollable(t->head, false);
    lv_obj_set_scrollbar_mode(t->head, LV_SCROLLBAR_MODE_OFF);
    t->body = ui_list(t->root);
    lv_obj_add_style(t->body, &styles[TABLE_BODY], 0);
    lv_obj_set_width(t->body, lv_pct(100));
    lv_obj_set_height(t->body, 0);
    lv_obj_set_flex_grow(t->body, 1);
    lv_obj_set_scroll_dir(t->body, LV_DIR_ALL);
    lv_obj_set_scroll_elastic(t->body, false);
    lv_obj_set_layout(t->body, LV_LAYOUT_NONE);
    lv_group_add_obj(gx_lv_group(), t->body);
    t->extent = ui_panel(t->body);
    lv_obj_remove_style_all(t->extent);
    lv_obj_set_clickable(t->extent, false);
    lv_obj_set_scrollable(t->extent, false);
    t->corner = ui_label(t->head, "#", UI_SECONDARY);
    lv_obj_set_floating(t->corner, true);
    lv_obj_add_style(t->corner, &styles[TABLE_NUMBER], 0);
    lv_obj_add_style(t->corner, &styles[TABLE_CORNER], 0);
    lv_obj_add_event_cb(t->body, body_event, LV_EVENT_ALL, t);
    return t;
}
lv_obj_t *ui_table_object(ui_table *t) { return t->root; }
lv_obj_t *ui_table_input(ui_table *t) { return t ? t->body : NULL; }
void ui_table_keys(ui_table *t, ui_table_key_fn key) { t->key = key; }
void ui_table_enabled(ui_table *t, int enabled) {
    t->enabled = !!enabled;
    t->dragging = 0;
    lv_obj_set_scrollable(t->body, !!enabled);
    if (enabled)
        lv_obj_remove_state(t->body, LV_STATE_DISABLED);
    else
        lv_obj_add_state(t->body, LV_STATE_DISABLED);
}
void ui_table_bind(ui_table *t, uint32_t rows, unsigned columns) {
    t->rows = rows;
    t->columns = columns < UI_TABLE_COLUMNS ? columns : UI_TABLE_COLUMNS;
    t->row = 0;
    t->column = 0;
    table_extent(t);
    lv_obj_scroll_to(t->body, 0, 0, LV_ANIM_OFF);
    ui_table_refresh(t);
}
void ui_table_heading(ui_table *t, unsigned col, const char *text) {
    char value[256];
    if (col >= UI_TABLE_COLUMNS)
        return;
    preview(value, sizeof(value), text);
    free(t->names[col]);
    t->header_dirty = 1;
    t->names[col] = malloc(strlen(value) + 1);
    if (t->names[col])
        strcpy(t->names[col], value);
}
void ui_table_callbacks(ui_table *t, ui_table_select_fn select, ui_table_sort_fn sort) {
    t->select = select;
    t->sort = sort;
}
void ui_table_column_width(ui_table *t, unsigned col, unsigned width) {
    if (col >= UI_TABLE_COLUMNS)
        return;
    width = width < 64 ? 64 : width > 480 ? 480 : width;
    if (t->widths[col] == width)
        return;
    t->widths[col] = width;
    table_extent(t);
    t->header_dirty = 1;
    table_viewport(t);
    lv_obj_invalidate(t->body);
}
unsigned ui_table_get_width(ui_table *t, unsigned col) {
    return col < UI_TABLE_COLUMNS ? t->widths[col] : 144;
}
void ui_table_density(ui_table *t) {
    uint32_t row = t->row;
    unsigned col = t->column, scale = gx_lv_density();
    int32_t sx = lv_obj_get_scroll_x(t->body) * (int32_t)scale / t->density;
    int32_t sy = lv_obj_get_scroll_y(t->body) * (int32_t)scale / t->density;
    t->density = scale;
    /* Scroll offsets are physical pixels, while widths and rows are logical.
     * Preserve the viewport when moving a window between display densities. */
    t->busy = 1;
    table_extent(t);
    lv_obj_update_layout(t->root);
    t->busy = 0;
    lv_obj_scroll_to(t->body, sx, sy, LV_ANIM_OFF);
    ui_table_select(t, row, col, 1);
    ui_table_refresh(t);
}
uint32_t ui_table_row(ui_table *t) { return t->row; }
unsigned ui_table_column(ui_table *t) { return t->column; }
int ui_table_focused(ui_table *t) { return lv_group_get_focused(gx_lv_group()) == t->body; }
void ui_table_focus(ui_table *t) { lv_group_focus_obj(t->body); }
void ui_table_page(ui_table *t, int direction) {
    uint32_t page = lv_obj_get_content_height(t->body) / gx_lv_px(UI_ROW_HEIGHT), row = t->row;
    if (!page)
        page = 1;
    if (direction < 0)
        row = row > page ? row - page : 0;
    else
        row += page;
    ui_table_select(t, row, t->column, 1);
}
void ui_table_sort_marker(ui_table *t, int col, int descending) {
    t->sorted = col;
    t->descending = descending;
    ui_table_refresh(t);
}
unsigned ui_table_widget_count(ui_table *t) { return 5 + t->pool_cols * 2; }

struct radio_list {
    lv_obj_t *root, *items[16];
    unsigned count, selected;
};
unsigned ui_radio_selected(lv_obj_t *list) {
    struct radio_list *r = lv_obj_get_user_data(list);
    return r->selected;
}
void ui_radio_select(lv_obj_t *list, unsigned selected) {
    struct radio_list *r = lv_obj_get_user_data(list);
    unsigned i, changed = r->selected != selected;
    if (selected >= r->count || lv_obj_has_state(r->items[selected], LV_STATE_DISABLED))
        return;
    r->selected = selected;
    for (i = 0; i < r->count; i++) {
        if (i == selected)
            lv_obj_add_state(r->items[i], LV_STATE_CHECKED);
        else
            lv_obj_remove_state(r->items[i], LV_STATE_CHECKED);
    }
    if (changed)
        lv_obj_send_event(list, LV_EVENT_VALUE_CHANGED, NULL);
}
static void radio_event(lv_event_t *event) {
    struct radio_list *r = lv_event_get_user_data(event);
    lv_obj_t *obj = lv_event_get_current_target_obj(event);
    lv_event_code_t code = lv_event_get_code(event);
    unsigned i;
    if (code == LV_EVENT_DELETE) {
        free(r);
        return;
    }
    for (i = 0; i < r->count && r->items[i] != obj; i++)
        ;
    if (i == r->count)
        return;
    if (code == LV_EVENT_VALUE_CHANGED)
        ui_radio_select(r->root, i);
    else if (code == LV_EVENT_KEY) {
        uint32_t key = lv_event_get_key(event);
        unsigned step, next = i;
        if (key != LV_KEY_UP && key != LV_KEY_DOWN && key != LV_KEY_LEFT && key != LV_KEY_RIGHT)
            return;
        for (step = 0; step < r->count; step++) {
            next = key == LV_KEY_UP || key == LV_KEY_LEFT ? (next ? next - 1 : r->count - 1)
                                                          : (next + 1) % r->count;
            if (!lv_obj_has_state(r->items[next], LV_STATE_DISABLED))
                break;
        }
        ui_radio_select(r->root, next);
        lv_group_focus_obj(r->items[next]);
    }
}
lv_obj_t *ui_radio_list(lv_obj_t *parent, const char *const *choices, unsigned count,
                        unsigned selected) {
    struct radio_list *r;
    unsigned i;
    if (!count || count > 16)
        return NULL;
    r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->root = ui_stack(parent, LV_FLEX_FLOW_COLUMN, UI_SPACE);
    r->count = count;
    r->selected = selected < count ? selected : 0;
    lv_obj_set_user_data(r->root, r);
    lv_obj_add_event_cb(r->root, radio_event, LV_EVENT_DELETE, r);
    for (i = 0; i < count; i++) {
        r->items[i] = ui_checkbox(r->root, choices[i]);
        lv_obj_add_style(r->items[i], &styles[RADIO], LV_PART_INDICATOR);
        lv_obj_add_style(r->items[i], &styles[RADIO_CHECKED], LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_add_style(r->items[i], &styles[RADIO_CHECKED],
                         LV_PART_INDICATOR | LV_STATE_CHECKED | LV_STATE_HOVERED);
        lv_obj_add_event_cb(r->items[i], radio_event, LV_EVENT_VALUE_CHANGED, r);
        lv_obj_add_event_cb(r->items[i], radio_event, LV_EVENT_KEY, r);
    }
    ui_radio_select(r->root, r->selected);
    return r->root;
}

static void overlay_geometry(void) {
    int32_t width, height;
    if (!overlay)
        return;
    width = lv_display_get_horizontal_resolution(lv_display_get_default());
    height = lv_display_get_vertical_resolution(lv_display_get_default());
    lv_obj_set_size(overlay->backdrop, width, height);
    if (!overlay->frame)
        return;
    if (!overlay->menu) {
        lv_obj_set_width(overlay->frame, gx_lv_px(overlay->width) < width - gx_lv_px(24)
                                             ? gx_lv_px(overlay->width)
                                             : width - gx_lv_px(24));
        lv_obj_set_style_max_height(overlay->frame, height - gx_lv_px(24), 0);
        lv_obj_center(overlay->frame);
    } else {
        int32_t w = gx_lv_px(320), h = gx_lv_px(overlay->count * UI_ROW_HEIGHT + 10),
                x = overlay->anchor_x * gx_lv_density(), y = overlay->anchor_y * gx_lv_density();
        if (w > width - gx_lv_px(16))
            w = width - gx_lv_px(16);
        if (h > height - gx_lv_px(16))
            h = height - gx_lv_px(16);
        if (x + w > width)
            x = width - w;
        if (y + h > height)
            y = height - h;
        lv_obj_set_pos(overlay->frame, x, y);
        lv_obj_set_size(overlay->frame, w, h);
    }
}
static ui_dialog *overlay_begin(void) {
    ui_dialog *d;
    if (overlay)
        return NULL;
    d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->previous = gx_lv_group();
    d->saved = lv_group_get_focused(d->previous);
    ui_focus_clear(d->previous);
    d->group = lv_group_create();
    gx_lv_set_group(d->group);
    d->backdrop = ui_panel(lv_layer_top());
    lv_obj_add_style(d->backdrop, &styles[OVERLAY], 0);
    lv_obj_set_scrollable(d->backdrop, false);
    lv_obj_set_click_focusable(d->backdrop, false);
    overlay = d;
    overlay_geometry();
    return d;
}
int ui_overlay_close_for(void *user) {
    if (!overlay || overlay->user != user)
        return 0;
    ui_overlay_close();
    return 1;
}
void ui_overlay_close(void) {
    ui_dialog *d = overlay;
    if (!d)
        return;
    overlay = NULL;
    gx_lv_set_group(d->previous);
    lv_obj_delete(d->backdrop);
    lv_group_delete(d->group);
    if (d->saved && lv_obj_is_valid(d->saved) && lv_obj_get_group(d->saved) == d->previous)
        lv_group_focus_obj(d->saved);
    free(d);
}
int ui_overlay_active(void) { return overlay != NULL; }
static void outside(lv_event_t *event) {
    if (overlay && overlay->menu && lv_event_get_target_obj(event) == overlay->backdrop)
        ui_overlay_close();
}
static void menu_choose(lv_event_t *event) {
    unsigned action = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    ui_menu_fn callback = overlay->menu_action;
    void *user = overlay->user;
    ui_overlay_close();
    if (callback)
        callback(action, user);
}
static void menu_key(lv_event_t *event) {
    uint32_t key = lv_event_get_key(event);
    if (key == LV_KEY_DOWN)
        lv_group_focus_next(gx_lv_group());
    else if (key == LV_KEY_UP)
        lv_group_focus_prev(gx_lv_group());
    else if (key == LV_KEY_HOME || key == LV_KEY_END) {
        unsigned i, count = lv_obj_get_child_count(overlay->frame);
        for (i = 0; i < count; i++) {
            unsigned index = key == LV_KEY_HOME ? i : count - i - 1;
            lv_obj_t *item = lv_obj_get_child(overlay->frame, index);
            if (!lv_obj_has_state(item, LV_STATE_DISABLED)) {
                lv_group_focus_obj(item);
                break;
            }
        }
    }
}
int ui_menu_open(lv_obj_t *anchor, const struct ui_menu_item *items, unsigned count,
                 ui_menu_fn callback, void *user) {
    ui_dialog *d = overlay_begin();
    lv_area_t area;
    int32_t x, y, w, h, screen_w, screen_h;
    unsigned i;
    lv_obj_t *first = NULL;
    if (!d)
        return 0;
    d->menu = 1;
    d->menu_action = callback;
    d->user = user;
    d->count = count;
    lv_obj_add_event_cb(d->backdrop, outside, LV_EVENT_PRESSED, NULL);
    d->frame = ui_panel(d->backdrop);
    lv_obj_add_style(d->frame, &styles[FRAME], 0);
    lv_obj_set_flex_flow(d->frame, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_style(d->frame, &styles[MENU_FRAME], 0);
    screen_w = lv_display_get_horizontal_resolution(lv_display_get_default());
    screen_h = lv_display_get_vertical_resolution(lv_display_get_default());
    lv_obj_get_coords(anchor, &area);
    w = gx_lv_px(320);
    if (w > screen_w - gx_lv_px(16))
        w = screen_w - gx_lv_px(16);
    d->anchor_x = area.x1 / (int32_t)gx_lv_density();
    d->anchor_y = (area.y2 + 1) / (int32_t)gx_lv_density();
    h = gx_lv_px(count * UI_ROW_HEIGHT + 10);
    if (h > screen_h - gx_lv_px(16))
        h = screen_h - gx_lv_px(16);
    x = area.x1;
    y = area.y2 + 1;
    if (x + w > screen_w)
        x = screen_w - w;
    if (y + h > screen_h)
        y = screen_h - h;
    lv_obj_set_pos(d->frame, x, y);
    lv_obj_set_size(d->frame, w, h);
    lv_obj_set_scroll_dir(d->frame, LV_DIR_VER);
    for (i = 0; i < count; i++) {
        lv_obj_t *b = ui_button(d->frame, NULL, UI_ROW);
        lv_obj_set_width(b, lv_pct(100));
        ui_row_icon_labels(b, items[i].icon ? items[i].icon : "minus", items[i].text,
                           items[i].shortcut ? items[i].shortcut : "");
        if (items[i].disabled)
            lv_obj_add_state(b, LV_STATE_DISABLED);
        else if (!first)
            first = b;
        lv_obj_add_event_cb(b, menu_choose, LV_EVENT_CLICKED, (void *)(uintptr_t)items[i].action);
        lv_obj_add_event_cb(b, menu_key, LV_EVENT_KEY, NULL);
    }
    if (first)
        lv_group_focus_obj(first);
    return 1;
}
static void dialog_choose(lv_event_t *event) {
    ui_dialog_submit(overlay, (unsigned)(uintptr_t)lv_event_get_user_data(event));
}
void ui_dialog_submit(ui_dialog *d, unsigned action) {
    if (!d || overlay != d)
        return;
    int close = d->action ? d->action(d, action, d->user) : 1;
    if (close && overlay == d)
        ui_overlay_close();
}
void ui_dialog_keys(ui_dialog *d, ui_dialog_key_fn key) {
    if (d && overlay == d)
        d->key = key;
}
ui_dialog *ui_dialog_open(const char *title, unsigned width, ui_dialog_fn callback, void *user) {
    ui_dialog *d = overlay_begin();
    if (!d)
        return NULL;
    d->width = width;
    d->action = callback;
    d->user = user;
    d->frame = ui_stack(d->backdrop, LV_FLEX_FLOW_COLUMN, UI_SPACE);
    lv_obj_add_style(d->frame, &styles[FRAME], 0);
    ui_label(d->frame, title, UI_HEADING);
    d->body = ui_stack(d->frame, LV_FLEX_FLOW_COLUMN, UI_SPACE);
    d->footer = ui_stack(d->frame, LV_FLEX_FLOW_ROW, UI_SPACE);
    lv_obj_set_flex_align(d->footer, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    overlay_geometry();
    return d;
}
lv_obj_t *ui_dialog_body(ui_dialog *d) { return d->body; }
lv_obj_t *ui_dialog_button(ui_dialog *d, const char *text, int primary, unsigned action) {
    lv_obj_t *b = ui_button(d->footer, text, primary ? UI_PRIMARY : UI_BUTTON);
    lv_obj_add_event_cb(b, dialog_choose, LV_EVENT_CLICKED, (void *)(uintptr_t)action);
    if (primary) {
        d->has_default = 1;
        d->default_action = action;
    }
    return b;
}
void ui_message(const char *title, const char *message) {
    ui_dialog *d = ui_dialog_open(title, 440, NULL, NULL);
    lv_obj_t *label;
    if (!d)
        return;
    label = ui_label(d->body, message, UI_BODY);
    lv_obj_set_width(label, lv_pct(100));
    lv_group_focus_obj(ui_dialog_button(d, "OK", 1, 0));
}
static lv_obj_t *overlay_input(void) {
    lv_obj_t *obj = overlay ? lv_group_get_focused(overlay->group) : NULL;
    return obj && lv_obj_check_type(obj, &lv_textarea_class) ? obj : NULL;
}
int ui_overlay_text(uint32_t scalar) {
    lv_obj_t *input;
    char text[5];
    unsigned n;
    if (!overlay)
        return 0;
    input = overlay_input();
    if (!input || scalar < 32 || scalar == 127)
        return 1;
    if (scalar < 128) {
        text[0] = scalar;
        n = 1;
    } else if (scalar < 2048) {
        text[0] = 0xc0 | (scalar >> 6);
        text[1] = 0x80 | (scalar & 63);
        n = 2;
    } else if (scalar < 65536 && !(scalar >= 0xd800 && scalar <= 0xdfff)) {
        text[0] = 0xe0 | (scalar >> 12);
        text[1] = 0x80 | ((scalar >> 6) & 63);
        text[2] = 0x80 | (scalar & 63);
        n = 3;
    } else if (scalar >= 65536UL && scalar <= 0x10ffffUL) {
        text[0] = 0xf0 | (scalar >> 18);
        text[1] = 0x80 | ((scalar >> 12) & 63);
        text[2] = 0x80 | ((scalar >> 6) & 63);
        text[3] = 0x80 | (scalar & 63);
        n = 4;
    } else
        return 1;
    text[n] = 0;
    if (overlay->replace)
        lv_textarea_set_text(input, "");
    overlay->replace = 0;
    lv_textarea_add_text(input, text);
    return 1;
}
int ui_overlay_action(unsigned action) {
    lv_obj_t *input;
    char buffer[2048];
    if (!overlay)
        return 0;
    input = overlay_input();
    if (!input)
        return 1;
    if (action == GX_ACTION_SELECT_ALL) {
        const unsigned char *s = (const unsigned char *)lv_textarea_get_text(input);
        unsigned n = 0;
        for (; *s; s++)
            if ((*s & 192) != 128)
                n++;
        lv_label_set_text_selection_start(lv_textarea_get_label(input), 0);
        lv_label_set_text_selection_end(lv_textarea_get_label(input), n);
        overlay->replace = 1;
    } else if (action == GX_ACTION_PASTE) {
        if (!gx_lv_clipboard(buffer, sizeof(buffer), 0) && !strpbrk(buffer, "\r\n\t")) {
            if (overlay->replace)
                lv_textarea_set_text(input, "");
            overlay->replace = 0;
            lv_textarea_add_text(input, buffer);
        }
    } else if (action == GX_ACTION_COPY || action == GX_ACTION_CUT) {
        const char *s = lv_textarea_get_text(input);
        if (strlen(s) < sizeof(buffer)) {
            strcpy(buffer, s);
            if (!gx_lv_clipboard(buffer, sizeof(buffer), 1) && action == GX_ACTION_CUT) {
                lv_textarea_set_text(input, "");
                overlay->replace = 0;
            }
        }
    }
    return 1;
}
int ui_overlay_key(unsigned code, int down, unsigned modifiers) {
    lv_obj_t *input;
    if (!overlay)
        return 0;
    if (code == 41) {
        if (down) {
            if (overlay->menu)
                ui_overlay_close();
            else
                ui_dialog_submit(overlay, 0);
        }
        return 1;
    }
    if (overlay->key && overlay->key(overlay, code, down, modifiers, overlay->user))
        return 1;
    input = overlay_input();
    if (code == 40 && input && overlay->has_default) {
        if (down)
            ui_dialog_submit(overlay, overlay->default_action);
        return 1;
    }
    if (down && input &&
        (code == 42 || code == 76 || code == 79 || code == 80 || code == 74 || code == 77)) {
        if (overlay->replace && (code == 42 || code == 76))
            lv_textarea_set_text(input, "");
        overlay->replace = 0;
        lv_textarea_clear_selection(input);
    }
    /* Suppress app-level shortcuts while the overlay owns keyboard focus. */
    return modifiers & 1 || (code >= 58 && code <= 69);
}
void ui_data_density(void) {
    unsigned i;
    for (i = 0; i < STYLE_COUNT; i++) {
        if (ready)
            lv_style_reset(&styles[i]);
        else
            lv_style_init(&styles[i]);
    }
    lv_style_set_pad_all(&styles[TABLE_ROOT], 0);
    lv_style_set_pad_row(&styles[TABLE_ROOT], 0);
    lv_style_set_height(&styles[TABLE_HEAD], gx_lv_px(UI_CONTROL));
    lv_style_set_pad_all(&styles[TABLE_HEAD], 0);
    lv_style_set_border_width(&styles[TABLE_HEAD], 0);
    lv_style_set_radius(&styles[TABLE_HEAD], 0);
    lv_style_set_pad_all(&styles[TABLE_BODY], 0);
    lv_style_set_pad_row(&styles[TABLE_BODY], 0);
    lv_style_set_border_width(&styles[TABLE_BODY], 0);
    lv_style_set_radius(&styles[TABLE_BODY], 0);
    lv_style_set_height(&styles[TABLE_CELL], gx_lv_px(UI_ROW_HEIGHT));
    lv_style_set_pad_hor(&styles[TABLE_CELL], gx_lv_px(8));
    lv_style_set_pad_ver(&styles[TABLE_CELL], gx_lv_px(3));
    lv_style_set_border_width(&styles[TABLE_CELL], gx_lv_px(1));
    lv_style_set_border_side(&styles[TABLE_CELL], LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_color(&styles[TABLE_CELL], ui_colour(UI_BORDER));
    lv_style_set_height(&styles[TABLE_NUMBER], gx_lv_px(UI_ROW_HEIGHT));
    lv_style_set_width(&styles[TABLE_NUMBER], gx_lv_px(NUMBER_WIDTH));
    lv_style_set_pad_hor(&styles[TABLE_NUMBER], gx_lv_px(6));
    lv_style_set_pad_ver(&styles[TABLE_NUMBER], gx_lv_px(4));
    lv_style_set_bg_opa(&styles[TABLE_NUMBER], LV_OPA_COVER);
    lv_style_set_bg_color(&styles[TABLE_NUMBER], ui_colour(UI_PANEL));
    lv_style_set_text_align(&styles[TABLE_NUMBER], LV_TEXT_ALIGN_RIGHT);
    lv_style_set_height(&styles[TABLE_CORNER], gx_lv_px(UI_CONTROL));
    lv_style_set_bg_opa(&styles[TABLE_SELECTED], LV_OPA_COVER);
    lv_style_set_bg_color(&styles[TABLE_SELECTED], lv_color_hex(0x303d52UL));
    lv_style_set_border_side(&styles[TABLE_SELECTED], LV_BORDER_SIDE_FULL);
    lv_style_set_border_color(&styles[TABLE_SELECTED], ui_colour(UI_ACCENT));
    lv_style_set_radius(&styles[TABLE_HEADER], 0);
    lv_style_set_height(&styles[TABLE_HEADER], gx_lv_px(UI_CONTROL));
    lv_style_set_min_width(&styles[TABLE_HEADER], 0);
    lv_style_set_pad_hor(&styles[TABLE_HEADER], gx_lv_px(8));
    lv_style_set_pad_ver(&styles[TABLE_HEADER], gx_lv_px(4));
    lv_style_set_border_side(&styles[TABLE_HEADER], LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_RIGHT);
    lv_style_set_border_color(&styles[TABLE_HEADER], ui_colour(UI_BORDER));
    lv_style_set_bg_opa(&styles[TABLE_HEADER_ACTIVE], LV_OPA_COVER);
    lv_style_set_bg_color(&styles[TABLE_HEADER_ACTIVE], lv_color_hex(0x303d52UL));
    lv_style_set_height(&styles[TABLE_HEADER_TEXT], lv_font_get_line_height(ui_font(UI_SECONDARY)));
    lv_style_set_radius(&styles[RADIO], LV_RADIUS_CIRCLE);
    lv_style_set_radius(&styles[RADIO_CHECKED], LV_RADIUS_CIRCLE);
    lv_style_set_bg_image_src(&styles[RADIO_CHECKED], NULL);
    lv_style_set_bg_color(&styles[RADIO_CHECKED], ui_colour(UI_ACCENT));
    lv_style_set_bg_opa(&styles[RADIO_CHECKED], LV_OPA_COVER);
    lv_style_set_border_width(&styles[RADIO_CHECKED], gx_lv_px(5));
    lv_style_set_border_color(&styles[RADIO_CHECKED], ui_colour(UI_CANVAS));
    lv_style_set_outline_width(&styles[RADIO_CHECKED], gx_lv_px(1));
    lv_style_set_outline_color(&styles[RADIO_CHECKED], ui_colour(UI_ACCENT));
    lv_style_set_outline_pad(&styles[RADIO_CHECKED], -gx_lv_px(1));
    lv_style_set_pad_all(&styles[OVERLAY], 0);
    lv_style_set_border_width(&styles[OVERLAY], 0);
    lv_style_set_radius(&styles[OVERLAY], 0);
    lv_style_set_bg_opa(&styles[OVERLAY], LV_OPA_TRANSP);
    /* The top layer is a sibling of the active screen, so it cannot inherit
     * the screen's Graphite typography or ink. */
    lv_style_set_text_font(&styles[OVERLAY], ui_font(UI_BODY));
    lv_style_set_text_color(&styles[OVERLAY], ui_colour(UI_TEXT));
    lv_style_set_bg_opa(&styles[FRAME], LV_OPA_COVER);
    lv_style_set_bg_color(&styles[FRAME], ui_colour(UI_PANEL));
    lv_style_set_border_width(&styles[FRAME], gx_lv_px(1));
    lv_style_set_border_color(&styles[FRAME], ui_colour(UI_BORDER));
    lv_style_set_radius(&styles[FRAME], gx_lv_px(4));
    lv_style_set_pad_all(&styles[FRAME], gx_lv_px(UI_INSET));
    lv_style_set_pad_all(&styles[MENU_FRAME], gx_lv_px(4));
    lv_style_set_pad_row(&styles[MENU_FRAME], 0);
    ready = 1;
    if (overlay) {
        overlay->replace = 0;
        overlay_geometry();
    }
}
void ui_data_close(void) {
    unsigned i;
    ui_overlay_close();
    if (!ready)
        return;
    for (i = 0; i < STYLE_COUNT; i++)
        lv_style_reset(&styles[i]);
    ready = 0;
}
static void overlay_inputs_resize(lv_obj_t *obj) {
    unsigned i;
    if (lv_obj_check_type(obj, &lv_textarea_class))
        ui_input_height(obj, UI_CONTROL);
    for (i = 0; i < lv_obj_get_child_count(obj); i++)
        overlay_inputs_resize(lv_obj_get_child(obj, i));
}
void ui_data_resize(void) {
    if (!overlay || !overlay->frame)
        return;
    overlay_inputs_resize(overlay->frame);
    overlay_geometry();
    lv_obj_update_layout(overlay->frame);
    overlay_geometry();
}
void ui_overlay_input_event(lv_event_t *event) {
    lv_event_code_t code = lv_event_get_code(event);
    if (overlay &&
        (code == LV_EVENT_PRESSED || code == LV_EVENT_DEFOCUSED || code == LV_EVENT_FOCUSED))
        overlay->replace = 0;
}
