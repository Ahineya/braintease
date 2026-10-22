/* SPDX-License-Identifier: MIT */
#include "ui-editor.h"
#include <gx.h>
#include "src/misc/lv_area_private.h"
#include "../../runtime/include/ripple_unicode.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* Display rows contain octet ranges into the shared engine. Neither wrapping
 * nor drawing inserts line endings into the document. Visible text runs stay
 * alive until LVGL has completed every strip of a refresh. */
#define EDITOR_WORK 4096UL
#define EDITOR_PAINT_ROWS 192
#define EDITOR_RUN_LIMIT 4096
#define EDITOR_TEXT_LIMIT 65536UL
#define EDITOR_CLIPBOARD (GX_CLIPBOARD_MAX + 1UL)
struct editor_row {
    unsigned long start, end, logical, column;
    int32_t width;
};
struct editor_run { unsigned long at, length; int32_t x, width; unsigned style; };
struct editor_paint {
    char *text;
    struct editor_run *runs;
    unsigned long length, capacity, first, last, row;
    unsigned count, run_capacity;
};
struct editor_syntax {
    unsigned long row, left, generation;
    unsigned valid;
    unsigned char kinds[512];
};
struct ui_editor {
    lv_obj_t *root, *extent;
    lv_timer_t *work;
    TextBuffer *buffer;
    const lv_font_t *font;
    struct editor_row *rows;
    struct editor_paint paint[EDITOR_PAINT_ROWS];
    struct editor_syntax syntax[16];
    unsigned next_syntax;
    unsigned long count, capacity, cursor, anchor, generation;
    unsigned long scan, scan_start, scan_row, scan_column, row_column;
    int32_t scan_width, max_width, width, height, line_height, space, goal;
    int32_t ascii[128];
    int32_t paint_sx, paint_sy, paint_width, paint_height;
    unsigned long paint_generation;
    unsigned long painted_cursor, painted_anchor, painted_generation;
    int32_t caret_x, caret_y, caret_height;
    unsigned caret_valid;
    unsigned density, wrap, complete, busy, dragging, paint_valid, paint_count, reveal, affinity;
    const char *error;
};
static void editor_layout(ui_editor *e);
static void editor_reveal(ui_editor *e);
static void paint_clear(ui_editor *e) {
    unsigned i;
    for (i = 0; i < EDITOR_PAINT_ROWS; i++) {
        free(e->paint[i].text);
        free(e->paint[i].runs);
        memset(&e->paint[i], 0, sizeof(e->paint[i]));
    }
    e->paint_valid = 0;
}
static uint32_t scalar(TextBuffer *b, unsigned long at, unsigned long end, unsigned long *next) {
    unsigned char bytes[4];
    size_t n = 0, used = 0;
    while (n < 4 && at + n < end) { bytes[n] = text_byte(b, at + n); n++; }
    if (!n) { *next = at; return RU_REPLACEMENT; }
    {
        uint32_t cp = ru_next(bytes, n, &used);
        *next = at + used;
        return cp;
    }
}
/* Same shared Unicode break tables as TextBuffer. Walking forward from a known
 * boundary avoids re-searching the beginning of a very long Unicode line for
 * every glyph. Storage and public caret mutations still belong to TextBuffer. */
static unsigned long grapheme(ui_editor *e, unsigned long at, unsigned long end,
                              int32_t x, int32_t *advance, unsigned *columns) {
    struct ru_break state = {0, 0, 0, 0, 0, 0};
    unsigned long p = at, next;
    int32_t width = 0;
    unsigned cells = 0;
    int ch = text_byte(e->buffer, at);
    if (ch == '\t') {
        int32_t tab = e->space * 4;
        *advance = tab - x % tab;
        *columns = 4 - (unsigned)(e->scan_column % 4);
        return at + 1;
    }
    while (p < end) {
        uint32_t cp = scalar(e->buffer, p, end, &next);
        int w;
        if (!ru_join(&state, cp) && p != at)
            break;
        w = ru_width(cp);
        if (w > 0 && (unsigned)w > cells)
            cells = w;
        if (cp == 0xfe0f || cp == 0x20e3 || state.regional >= 2)
            cells = 2;
        if (cp < 32 || (cp >= 127 && cp < 160))
            cp = RU_REPLACEMENT;
        width += cp < 128 ? e->ascii[cp] : lv_font_get_glyph_width(e->font, cp, 0);
        p = next;
    }
    *advance = width > 0 ? width : e->space;
    *columns = cells ? cells : 1;
    return p;
}
static unsigned long next_cluster(ui_editor *e, unsigned long at, unsigned long end,
                                  int32_t x, int32_t *width, unsigned long column,
                                  unsigned *columns) {
    unsigned long next = grapheme(e, at, end, x, width, columns);
    if (text_byte(e->buffer, at) == '\t')
        *columns = 4 - column % 4;
    return next;
}
static void metrics(ui_editor *e) {
    unsigned i;
    e->density = gx_lv_density();
    e->font = ui_font(UI_BODY);
    e->line_height = lv_font_get_line_height(e->font) + gx_lv_px(3);
    e->space = lv_font_get_glyph_width(e->font, ' ', 0);
    if (e->space < 1)
        e->space = gx_lv_px(4);
    for (i = 0; i < 128; i++)
        e->ascii[i] = lv_font_get_glyph_width(e->font, i < 32 ? RU_REPLACEMENT : i, 0);
    e->width = lv_obj_get_content_width(e->root) - gx_lv_px(2);
    e->height = lv_obj_get_content_height(e->root);
    if (e->width < gx_lv_px(16))
        e->width = gx_lv_px(16);
}
static int append_row(ui_editor *e, unsigned long end) {
    if (e->count == e->capacity) {
        unsigned long cap = e->capacity ? e->capacity * 2 : 128;
        struct editor_row *rows;
        if (cap > TEXT_MAX + TEXT_LINES)
            cap = TEXT_MAX + TEXT_LINES;
        if (cap <= e->capacity) {
            e->error = "Not enough memory for text layout.";
            return 0;
        }
        rows = realloc(e->rows, cap * sizeof(*rows));
        if (!rows) { e->error = "Not enough memory for text layout."; return 0; }
        e->rows = rows;
        e->capacity = cap;
    }
    e->rows[e->count++] = (struct editor_row){e->scan_start, end, e->scan_row,
                                             e->row_column, e->scan_width};
    if (e->scan_width > e->max_width)
        e->max_width = e->scan_width;
    e->scan_start = end;
    e->row_column = e->scan_column;
    e->scan_width = 0;
    return 1;
}
static void extent(ui_editor *e) {
    unsigned long rows = e->count;
    int32_t h, w;
    if (!e->complete && e->buffer) {
        unsigned long pending = text_lines(e->buffer) - e->scan_row;
        if (pending > rows)
            rows = pending;
        if (rows < e->count + 1)
            rows = e->count + 1;
    }
    h = rows > (unsigned long)(INT32_MAX / e->line_height) ? INT32_MAX : rows * e->line_height;
    w = e->wrap ? e->width : e->max_width + gx_lv_px(8);
    if (w < e->width)
        w = e->width;
    if (h < e->height)
        h = e->height;
    lv_obj_set_size(e->extent, w, h);
}
static void restart(ui_editor *e, unsigned long first) {
    unsigned long keep = 0;
    if (!e->buffer)
        return;
    while (keep < e->count && e->rows[keep].logical < first)
        keep++;
    e->count = keep;
    e->scan_row = first < text_lines(e->buffer) ? first : text_lines(e->buffer) - 1;
    e->scan = e->scan_start = text_start(e->buffer, e->scan_row);
    e->scan_column = e->row_column = 0;
    e->scan_width = e->max_width = 0;
    for (keep = 0; keep < e->count; keep++)
        if (e->rows[keep].width > e->max_width)
            e->max_width = e->rows[keep].width;
    e->complete = 0;
    e->error = NULL;
    e->generation = text_generation(e->buffer);
    paint_clear(e);
    if (e->work)
        lv_timer_resume(e->work);
}
/* A layout batch does bounded grapheme work; an unusually long single
 * grapheme is kept atomic. No font lookup or character widget per whole file. */
static void step_layout(ui_editor *e, unsigned long budget) {
    unsigned long count = 0, before = e->count;
    if (e->busy || e->complete || !e->buffer)
        return;
    e->busy = 1;
    while (count < budget && !e->complete) {
        unsigned long end = text_endline(e->buffer, e->scan_row);
        if (e->scan >= end) {
            if (!append_row(e, end)) { e->complete = 1; break; }
            if (++e->scan_row >= text_lines(e->buffer)) {
                e->complete = 1;
                break;
            }
            e->scan = e->scan_start = text_start(e->buffer, e->scan_row);
            e->scan_column = e->row_column = 0;
            count++;
        } else {
            int32_t width;
            unsigned columns;
            unsigned long next = next_cluster(e, e->scan, end, e->scan_width,
                                               &width, e->scan_column, &columns);
            if (e->wrap && e->scan_width && width > e->width - e->scan_width) {
                if (!append_row(e, e->scan)) { e->complete = 1; break; }
                /* A tab is anchored to its displayed row's pixel origin. */
                next = next_cluster(e, e->scan, end, 0, &width, e->scan_column, &columns);
            }
            if (width > INT32_MAX - e->scan_width)
                width = INT32_MAX - e->scan_width;
            e->scan_width += width;
            e->scan_column += columns;
            e->scan = next;
            count++;
        }
    }
    if (e->complete && e->work)
        lv_timer_pause(e->work);
    extent(e);
    e->busy = 0;
    if (e->count != before) {
        e->paint_valid = 0;
        lv_obj_invalidate(e->root);
    }
}
static void editor_layout(ui_editor *e) {
    int32_t width = lv_obj_get_content_width(e->root) - gx_lv_px(2);
    int32_t height = lv_obj_get_content_height(e->root);
    if (width < gx_lv_px(16))
        width = gx_lv_px(16);
    if ((e->wrap && width != e->width) || gx_lv_density() != e->density || ui_font(UI_BODY) != e->font) {
        metrics(e);
        restart(e, 0);
    } else if (width != e->width || height != e->height) {
        e->width = width;
        e->height = height;
        e->paint_valid = 0;
        extent(e);
    }
    if (e->buffer && text_generation(e->buffer) != e->generation) {
        unsigned long bounds[2];
        text_damage(e->buffer, bounds);
        restart(e, bounds[1] ? bounds[0] : 0);
    }
}
static unsigned long cursor_row(ui_editor *e, unsigned long cursor) {
    unsigned long lo = 0, hi;
    /* Explicit End/file navigation may demand layout up to its destination.
     * Ordinary paint and idle work remain bounded. */
    while (!e->complete && (!e->count || e->rows[e->count - 1].end < cursor))
        step_layout(e, EDITOR_WORK);
    hi = e->count;
    while (lo < hi) {
        unsigned long mid = lo + (hi - lo) / 2;
        if (e->rows[mid].start <= cursor)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo)
        lo--;
    if (e->affinity && lo && e->rows[lo].start == cursor && e->rows[lo - 1].end == cursor)
        lo--;
    return lo;
}
static int32_t row_x(ui_editor *e, const struct editor_row *r, unsigned long at) {
    unsigned long p = r->start, column = r->column;
    int32_t x = 0;
    while (p < r->end && p < at) {
        int32_t w;
        unsigned columns;
        p = next_cluster(e, p, r->end, x, &w, column, &columns);
        column += columns;
        x += w;
    }
    return x;
}
static unsigned long row_hit(ui_editor *e, const struct editor_row *r, int32_t x) {
    unsigned long p = r->start, column = r->column;
    int32_t pen = 0;
    while (p < r->end) {
        int32_t w;
        unsigned columns;
        unsigned long next = next_cluster(e, p, r->end, pen, &w, column, &columns);
        if (x < pen + (w + 1) / 2)
            break;
        pen += w;
        column += columns;
        p = next;
    }
    return p;
}
static void editor_reveal(ui_editor *e) {
    unsigned long row;
    int32_t x, y, sx, sy, h;
    if (!e->buffer || !e->reveal)
        return;
    row = cursor_row(e, e->cursor);
    if (!e->count)
        return;
    e->reveal = 0;
    x = row_x(e, &e->rows[row], e->cursor);
    y = row * e->line_height;
    sx = lv_obj_get_scroll_x(e->root);
    sy = lv_obj_get_scroll_y(e->root);
    h = lv_obj_get_content_height(e->root);
    if (e->wrap)
        sx = 0;
    else if (x < sx)
        sx = x;
    else if (x + gx_lv_px(3) > sx + e->width)
        sx = x + gx_lv_px(3) - e->width;
    if (y < sy)
        sy = y;
    else if (y + e->line_height > sy + h)
        sy = y + e->line_height - h;
    lv_obj_scroll_to(e->root, sx, sy, LV_ANIM_OFF);
}
static void work(lv_timer_t *timer) {
    ui_editor *e = lv_timer_get_user_data(timer);
    editor_layout(e);
    step_layout(e, EDITOR_WORK);
    editor_reveal(e);
}
static void notify(ui_editor *e, int reveal) {
    int32_t sx = lv_obj_get_scroll_x(e->root), sy = lv_obj_get_scroll_y(e->root);
    int32_t x, y, h;
    e->reveal |= reveal;
    editor_layout(e);
    step_layout(e, EDITOR_WORK);
    editor_reveal(e);
    if (e->caret_valid && e->painted_generation == e->generation &&
        e->painted_cursor == e->painted_anchor && e->cursor == e->anchor &&
        sx == lv_obj_get_scroll_x(e->root) && sy == lv_obj_get_scroll_y(e->root) &&
        ui_editor_caret(e, &x, &y, &h)) {
        lv_area_t area, damage;
        lv_obj_get_content_coords(e->root, &area);
        damage = (lv_area_t){area.x1 + e->caret_x, area.y1 + e->caret_y,
                             area.x1 + e->caret_x + gx_lv_px(2),
                             area.y1 + e->caret_y + e->caret_height};
        lv_obj_invalidate_area(e->root, &damage);
        damage = (lv_area_t){area.x1 + x, area.y1 + y, area.x1 + x + gx_lv_px(2), area.y1 + y + h};
        lv_obj_invalidate_area(e->root, &damage);
    } else
        lv_obj_invalidate(e->root);
    lv_obj_send_event(e->root, LV_EVENT_VALUE_CHANGED, NULL);
}
void ui_editor_selection(ui_editor *e, unsigned long *bounds) {
    bounds[0] = e->anchor < e->cursor ? e->anchor : e->cursor;
    bounds[1] = e->anchor < e->cursor ? e->cursor : e->anchor;
}
unsigned long ui_editor_cursor(ui_editor *e) { return e ? e->cursor : 0; }
void ui_editor_select(ui_editor *e, unsigned long anchor, unsigned long cursor) {
    if (!e || !e->buffer)
        return;
    e->anchor = text_boundary(e->buffer, anchor);
    e->cursor = text_boundary(e->buffer, cursor);
    e->affinity = 0;
    e->goal = -1;
    notify(e, 1);
}
static int replace_selection(ui_editor *e, const unsigned char *bytes, unsigned long length) {
    unsigned long bounds[2];
    int result;
    if (!e || !e->buffer || (length && !bytes))
        return -1;
    e->error = NULL;
    ui_editor_selection(e, bounds);
    text_begin(e->buffer, e->cursor);
    result = text_replace(e->buffer, bounds[0], bounds[1] - bounds[0], bytes, length);
    if (!result) {
        e->cursor = bounds[0] + length;
        e->anchor = e->cursor;
        e->affinity = 0;
    }
    text_end(e->buffer, e->cursor);
    e->goal = -1;
    notify(e, !result);
    return result;
}
int ui_editor_text(ui_editor *e, const char *text, unsigned long length) {
    return replace_selection(e, (const unsigned char *)text, length);
}
int ui_editor_codepoint(ui_editor *e, uint32_t cp) {
    unsigned char bytes[4];
    unsigned n;
    if (cp < 32 || (cp >= 127 && cp < 160) || cp > 0x10ffffUL || (cp >= 0xd800 && cp <= 0xdfff))
        return -1;
    n = ru_encode(cp, bytes);
    return replace_selection(e, bytes, n);
}
void ui_editor_changed(ui_editor *e) {
    if (!e || !e->buffer)
        return;
    e->cursor = text_boundary(e->buffer, e->cursor);
    e->anchor = text_boundary(e->buffer, e->anchor);
    e->affinity = 0;
    e->goal = -1;
    notify(e, 0);
}
static int copy_selection(ui_editor *e, int cut) {
    unsigned long bounds[2], n;
    char *bytes;
    int result;
    ui_editor_selection(e, bounds);
    n = bounds[1] - bounds[0];
    if (!n)
        return 0;
    if (n > GX_CLIPBOARD_MAX) {
        e->error = "The selection exceeds the 32 KiB clipboard limit.";
        return -1;
    }
    bytes = malloc((n + 1) * sizeof(*bytes));
    if (!bytes) { e->error = "Not enough memory for the clipboard."; return -1; }
    result = text_copy(e->buffer, bounds[0], n, (unsigned char *)bytes);
    bytes[n] = 0;
    if (!result)
        result = gx_lv_clipboard(bytes, n + 1, 1);
    free(bytes);
    if (!result && cut)
        result = replace_selection(e, NULL, 0);
    if (result)
        e->error = "The clipboard is unavailable.";
    return result;
}
int ui_editor_action(ui_editor *e, unsigned action) {
    char *bytes;
    int result;
    if (!e || !e->buffer)
        return 0;
    e->error = NULL;
    if (action == GX_ACTION_SELECT_ALL)
        ui_editor_select(e, 0, text_length(e->buffer));
    else if (action == GX_ACTION_COPY || action == GX_ACTION_CUT)
        copy_selection(e, action == GX_ACTION_CUT);
    else if (action == GX_ACTION_PASTE) {
        bytes = malloc(EDITOR_CLIPBOARD * sizeof(*bytes));
        if (!bytes)
            e->error = "Not enough memory for the clipboard.";
        else {
            result = gx_lv_clipboard(bytes, EDITOR_CLIPBOARD, 0);
            if (!result)
                ui_editor_text(e, bytes, strlen(bytes));
            else
                e->error = "The clipboard is unavailable or too large.";
            free(bytes);
        }
    } else
        return 0;
    if (e->error)
        lv_obj_send_event(e->root, LV_EVENT_VALUE_CHANGED, NULL);
    return 1;
}
static void move_cursor(ui_editor *e, unsigned long to, int shift) {
    e->cursor = text_boundary(e->buffer, to);
    if (!shift)
        e->anchor = e->cursor;
    notify(e, 1);
}
static int move_vertical(ui_editor *e, int delta, int shift) {
    unsigned long row = cursor_row(e, e->cursor), target;
    if (!e->count)
        return 1;
    if (e->goal < 0)
        e->goal = row_x(e, &e->rows[row], e->cursor);
    if (delta < 0)
        target = row >= (unsigned)-delta ? row + delta : 0;
    else {
        target = row + delta;
        while (!e->complete && e->count <= target)
            step_layout(e, EDITOR_WORK);
        if (target >= e->count)
            target = e->count - 1;
    }
    {
        unsigned long to = row_hit(e, &e->rows[target], e->goal);
        e->affinity = to == e->rows[target].end;
        move_cursor(e, to, shift);
    }
    return 1;
}
int ui_editor_key(ui_editor *e, unsigned key, int down, unsigned modifiers) {
    unsigned long bounds[2], to, row;
    int ctrl = modifiers & 1, shift = modifiers & 2, consumed = 1;
    if (!e || !e->buffer || !ui_editor_focused(e))
        return 0;
    if (ctrl && key == 4) { if (down) ui_editor_action(e, GX_ACTION_SELECT_ALL); }
    else if (ctrl && key == 6) { if (down) ui_editor_action(e, GX_ACTION_COPY); }
    else if (ctrl && key == 27) { if (down) ui_editor_action(e, GX_ACTION_CUT); }
    else if (ctrl && key == 25) { if (down) ui_editor_action(e, GX_ACTION_PASTE); }
    else if (ctrl && (key == 29 || key == 28)) {
        if (down) {
            long cursor = key == 28 || shift ? text_redo(e->buffer) : text_undo(e->buffer);
            if (cursor >= 0)
                e->cursor = e->anchor = cursor;
            e->affinity = 0;
            e->goal = -1;
            notify(e, cursor >= 0);
        }
    } else if (key == 79 || key == 80) {
        if (down) {
            ui_editor_selection(e, bounds);
            if (!shift && bounds[0] != bounds[1])
                to = bounds[key == 79];
            else if (ctrl)
                to = text_word(e->buffer, e->cursor, key == 79 ? 'w' : 'b', 1);
            else
                to = key == 79 ? text_next(e->buffer, e->cursor) : text_previous(e->buffer, e->cursor);
            e->goal = -1;
            e->affinity = 0;
            move_cursor(e, to, shift);
        }
    } else if (key == 81 || key == 82 || key == 75 || key == 78) {
        if (down) {
            int delta = (key == 75 || key == 78) ? e->height / e->line_height - 1 : 1;
            if (delta < 1)
                delta = 1;
            move_vertical(e, (key == 82 || key == 75) ? -delta : delta, shift);
        }
    } else if (key == 74 || key == 77) {
        if (down) {
            row = cursor_row(e, e->cursor);
            to = ctrl || !e->count ? (key == 74 ? 0 : text_length(e->buffer)) :
                 key == 74 ? e->rows[row].start : e->rows[row].end;
            e->goal = -1;
            e->affinity = !ctrl && key == 77;
            move_cursor(e, to, shift);
        }
    } else if (key == 42 || key == 76) {
        if (down) {
            ui_editor_selection(e, bounds);
            if (bounds[0] == bounds[1])
                e->anchor = key == 42 ? text_previous(e->buffer, e->cursor) : text_next(e->buffer, e->cursor);
            replace_selection(e, NULL, 0);
        }
    } else if (key == 40 || key == 88) {
        if (down) {
            unsigned long end = text_endline(e->buffer, 0);
            /* Preserve CRLF when the first existing line uses it. One
             * transactional replace also protects a selected range on failure. */
            if (text_byte(e->buffer, end) == '\r')
                ui_editor_text(e, "\r\n", 2);
            else
                ui_editor_text(e, "\n", 1);
        }
    } else if (key == 43 && !shift && !ctrl) {
        if (down)
            ui_editor_text(e, "\t", 1);
    } else
        consumed = 0;
    return consumed;
}
unsigned long ui_editor_hit(ui_editor *e, int32_t x, int32_t y) {
    unsigned long row;
    int32_t position;
    if (!e || !e->buffer)
        return 0;
    editor_layout(e);
    position = y + lv_obj_get_scroll_y(e->root);
    row = position > 0 ? (unsigned long)(position / e->line_height) : 0;
    while (!e->complete && e->count <= row)
        step_layout(e, EDITOR_WORK);
    if (!e->count)
        return 0;
    if (row >= e->count)
        row = e->count - 1;
    return row_hit(e, &e->rows[row], x + lv_obj_get_scroll_x(e->root));
}
int ui_editor_caret(ui_editor *e, int32_t *x, int32_t *y, int32_t *height) {
    unsigned long row;
    if (!e || !e->buffer)
        return 0;
    editor_layout(e);
    row = cursor_row(e, e->cursor);
    if (!e->count)
        return 0;
    *x = row_x(e, &e->rows[row], e->cursor) - lv_obj_get_scroll_x(e->root);
    *y = row * e->line_height - lv_obj_get_scroll_y(e->root);
    *height = e->line_height;
    return 1;
}
static int paint_append(struct editor_paint *p, const unsigned char *bytes, unsigned n) {
    unsigned long cap;
    char *text;
    if (p->length + n + 1 > EDITOR_TEXT_LIMIT)
        return 0;
    if (p->length + n + 1 > p->capacity) {
        cap = p->capacity ? p->capacity * 2 : 512;
        while (cap < p->length + n + 1)
            cap *= 2;
        text = realloc(p->text, cap * sizeof(*text));
        if (!text)
            return 0;
        p->text = text;
        p->capacity = cap;
    }
    memcpy(p->text + p->length, bytes, n);
    p->length += n;
    p->text[p->length] = 0;
    return 1;
}
static struct editor_run *paint_run(struct editor_paint *p, int32_t x, unsigned style) {
    struct editor_run *r;
    if (p->count && p->runs[p->count - 1].style == style &&
        p->runs[p->count - 1].x + p->runs[p->count - 1].width == x)
        return &p->runs[p->count - 1];
    if (p->count == p->run_capacity) {
        unsigned cap = p->run_capacity ? p->run_capacity * 2 : 16;
        struct editor_run *runs;
        if (cap > EDITOR_RUN_LIMIT)
            return NULL;
        runs = realloc(p->runs, cap * sizeof(*runs));
        if (!runs)
            return NULL;
        p->runs = runs;
        p->run_capacity = cap;
    }
    r = &p->runs[p->count++];
    *r = (struct editor_run){p->length, 0, x, 0, style};
    return r;
}
/* Several pixel-wrapped rows usually share one logical syntax chunk. Retain
 * those chunks across scrolling instead of lexing the same logical line once
 * for every visible display row. Styles come only from the shared engine. */
static unsigned syntax_style(ui_editor *e, unsigned long row, unsigned long column) {
    unsigned long left = column / 512UL * 512UL, styles[7], cells[512];
    struct editor_syntax *chunk;
    unsigned i;
    for (i = 0; i < 16; i++) {
        chunk = &e->syntax[i];
        if (chunk->valid && chunk->generation == e->generation && chunk->row == row && chunk->left == left)
            return chunk->kinds[column - left];
    }
    chunk = &e->syntax[e->next_syntax++ % 16];
    for (i = 0; i < 7; i++)
        styles[i] = (unsigned long)i << 24;
    text_render(e->buffer, row, left, 512, 4, styles, 0, 0, 0, cells);
    chunk->row = row;
    chunk->left = left;
    chunk->generation = e->generation;
    chunk->valid = 1;
    for (i = 0; i < 512; i++) {
        unsigned style = (unsigned)(cells[i] >> 24);
        chunk->kinds[i] = style <= 6 ? style : 0;
    }
    return chunk->kinds[column - left];
}
static void prepare_row(ui_editor *e, struct editor_paint *p, unsigned long row, int32_t sx) {
    const struct editor_row *r = &e->rows[row];
    unsigned long at = r->start, column = r->column;
    int32_t x = 0;
    p->length = p->count = 0;
    p->row = row;
    p->first = p->last = r->start;
    while (at < r->end && x <= sx + e->width) {
        int32_t width;
        unsigned columns;
        unsigned long next = next_cluster(e, at, r->end, x, &width, column, &columns);
        if (x + width >= sx && text_byte(e->buffer, at) != '\t') {
            struct editor_run *run;
            unsigned style;
            unsigned long q = at;
            style = syntax_style(e, r->logical, column);
            run = paint_run(p, x, style);
            if (!run)
                break;
            if (p->count == 1 && !run->length)
                p->first = at;
            /* Do not hand malformed UTF-8, C0 controls or terminal interned
             * glyph IDs to LVGL. Preserve the complete source in TextBuffer. */
            while (q < next) {
                unsigned char bytes[4];
                unsigned long after;
                uint32_t cp = scalar(e->buffer, q, next, &after);
                unsigned n;
                if (cp < 32 || (cp >= 127 && cp < 160))
                    cp = RU_REPLACEMENT;
                n = ru_encode(cp, bytes);
                if (!paint_append(p, bytes, n))
                    break;
                run->length += n;
                q = after;
            }
            run->width += width;
            p->last = next;
        }
        x += width;
        column += columns;
        at = next;
    }
}
static void prepare_paint(ui_editor *e) {
    int32_t sx = lv_obj_get_scroll_x(e->root), sy = lv_obj_get_scroll_y(e->root);
    int32_t height = lv_obj_get_content_height(e->root);
    unsigned long first = sy > 0 ? sy / e->line_height : 0;
    unsigned i;
    if (e->paint_valid && e->paint_sx == sx && e->paint_sy == sy &&
        e->paint_width == e->width && e->paint_height == height &&
        e->paint_generation == e->generation)
        return;
    e->paint_valid = 1;
    e->paint_sx = sx;
    e->paint_sy = sy;
    e->paint_width = e->width;
    e->paint_height = height;
    e->paint_generation = e->generation;
    e->paint_count = 0;
    for (i = 0; i < EDITOR_PAINT_ROWS; i++) {
        unsigned long row = first + i;
        e->paint[i].count = 0;
        if (row >= e->count || (int32_t)(row * e->line_height) - sy >= height)
            break;
        prepare_row(e, &e->paint[i], row, sx);
        e->paint_count++;
    }
    for (; i < EDITOR_PAINT_ROWS; i++)
        e->paint[i].count = 0;
}
static lv_color_t syntax_colour(unsigned style) {
    static const uint32_t colours[7] = {0xe0e1e5UL, 0x858b98UL, 0xb6a5e8UL, 0x9ac59fUL,
                                        0xd9b886UL, 0x9eb9dcUL, 0x82bfc2UL};
    return style ? lv_color_hex(colours[style]) : ui_colour(UI_TEXT);
}
static void draw(ui_editor *e, lv_layer_t *layer) {
    lv_area_t area, saved = layer->_clip_area, viewport, rectangle;
    lv_draw_rect_dsc_t selection, caret;
    lv_draw_label_dsc_t label;
    unsigned long bounds[2];
    int32_t sx, sy;
    unsigned i, j;
    lv_obj_get_content_coords(e->root, &area);
    if (!lv_area_intersect(&viewport, &saved, &area))
        return;
    layer->_clip_area = viewport;
    prepare_paint(e);
    ui_editor_selection(e, bounds);
    sx = lv_obj_get_scroll_x(e->root);
    sy = lv_obj_get_scroll_y(e->root);
    lv_draw_rect_dsc_init(&selection);
    selection.bg_opa = LV_OPA_COVER;
    selection.bg_color = lv_color_hex(ui_editor_focused(e) ? 0x344761UL : 0x30343cUL);
    lv_draw_rect_dsc_init(&caret);
    caret.bg_opa = LV_OPA_COVER;
    caret.bg_color = ui_colour(UI_ACCENT);
    lv_draw_label_dsc_init(&label);
    label.base.obj = e->root;
    label.font = e->font;
    label.flag = LV_TEXT_FLAG_EXPAND;
    label.text_size.y = lv_font_get_line_height(e->font);
    for (i = 0; i < e->paint_count; i++) {
        struct editor_paint *p = &e->paint[i];
        const struct editor_row *r;
        int32_t y;
        if (p->row >= e->count)
            continue;
        r = &e->rows[p->row];
        y = area.y1 + (int32_t)(p->row * e->line_height) - sy;
        if (y > viewport.y2 || y + e->line_height <= viewport.y1)
            continue;
        if (bounds[0] != bounds[1] && bounds[1] >= r->start && bounds[0] <= r->end) {
            unsigned long first = bounds[0] > r->start ? bounds[0] : r->start;
            unsigned long last = bounds[1] < r->end ? bounds[1] : r->end;
            int newline = bounds[1] > r->end && text_endline(e->buffer, r->logical) == r->end;
            rectangle = (lv_area_t){area.x1 + row_x(e, r, first) - sx, y,
                                    area.x1 + row_x(e, r, last) - sx - 1, y + e->line_height - 1};
            if (newline)
                rectangle.x2 += e->space;
            if (rectangle.x2 >= rectangle.x1)
                lv_draw_rect(layer, &selection, &rectangle);
        }
        for (j = 0; j < p->count; j++) {
            struct editor_run *run = &p->runs[j];
            if (!run->length)
                continue;
            rectangle = (lv_area_t){area.x1 + run->x - sx, y,
                                    area.x1 + run->x - sx + run->width - 1,
                                    y + e->line_height - 1};
            label.text = p->text + run->at;
            label.text_length = run->length;
            label.text_size.x = run->width;
            label.color = syntax_colour(run->style);
            lv_draw_label(layer, &label, &rectangle);
        }
    }
    if (ui_editor_focused(e) && !e->dragging) {
        int32_t x, y, h;
        if (ui_editor_caret(e, &x, &y, &h)) {
            rectangle = (lv_area_t){area.x1 + x, area.y1 + y, area.x1 + x + gx_lv_px(1) - 1,
                                    area.y1 + y + h - gx_lv_px(2) - 1};
            lv_draw_rect(layer, &caret, &rectangle);
            e->caret_x = x;
            e->caret_y = y;
            e->caret_height = h;
            e->caret_valid = 1;
        }
    } else
        e->caret_valid = 0;
    e->painted_cursor = e->cursor;
    e->painted_anchor = e->anchor;
    e->painted_generation = e->generation;
    layer->_clip_area = saved;
}
static void event(lv_event_t *ev) {
    ui_editor *e = lv_event_get_user_data(ev);
    lv_event_code_t code = lv_event_get_code(ev);
    if (code == LV_EVENT_DELETE) {
        if (e->work)
            lv_timer_delete(e->work);
        paint_clear(e);
        free(e->rows);
        free(e);
    } else if (code == LV_EVENT_DRAW_MAIN && e->buffer)
        draw(e, lv_event_get_layer(ev));
    else if (code == LV_EVENT_SIZE_CHANGED && !e->busy) {
        editor_layout(e);
        step_layout(e, EDITOR_WORK);
        e->paint_valid = 0;
    } else if (code == LV_EVENT_SCROLL) {
        unsigned long need;
        int32_t end = lv_obj_get_scroll_y(e->root) + lv_obj_get_content_height(e->root);
        need = end > 0 ? end / e->line_height + 2 : 2;
        /* Scrolling a large unloaded suffix fills only a bounded batch; its
         * remainder continues on the idle timer instead of blocking input. */
        if (need > e->count)
            step_layout(e, EDITOR_WORK);
        e->paint_valid = 0;
    } else if (code == LV_EVENT_FOCUSED || code == LV_EVENT_DEFOCUSED)
        lv_obj_invalidate(e->root);
    else if ((code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) && lv_indev_active()) {
        lv_point_t point;
        lv_area_t area;
        unsigned long at;
        lv_indev_get_point(lv_indev_active(), &point);
        lv_obj_get_content_coords(e->root, &area);
        at = ui_editor_hit(e, point.x - area.x1, point.y - area.y1);
        if (code == LV_EVENT_PRESSED) {
            e->anchor = at;
            e->dragging = 1;
            /* Text dragging selects. Trackpad wheel still uses this object's
             * ordinary scroll path once the pointer is released. */
            lv_obj_set_scrollable(e->root, false);
            ui_editor_focus(e);
        }
        if (e->dragging && (e->cursor != at || code == LV_EVENT_PRESSED)) {
            int32_t py = point.y - area.y1 + lv_obj_get_scroll_y(e->root);
            unsigned long row = py > 0 ? py / e->line_height : 0;
            e->cursor = at;
            e->affinity = row < e->count && at == e->rows[row].end;
            e->goal = -1;
            notify(e, code == LV_EVENT_PRESSING);
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST || code == LV_EVENT_INDEV_RESET) {
        e->dragging = 0;
        lv_obj_set_scrollable(e->root, true);
        lv_obj_invalidate(e->root);
    } else if (code == LV_EVENT_KEY) {
        /* The raw GX hook supplies modifiers and consumes normal editor keys.
         * This path also permits embedders with a plain LVGL keyboard. */
        uint32_t key = lv_event_get_key(ev);
        unsigned usb = key == LV_KEY_LEFT ? 80 : key == LV_KEY_RIGHT ? 79 :
                       key == LV_KEY_UP ? 82 : key == LV_KEY_DOWN ? 81 :
                       key == LV_KEY_HOME ? 74 : key == LV_KEY_END ? 77 :
                       key == LV_KEY_BACKSPACE ? 42 : key == LV_KEY_DEL ? 76 :
                       key == LV_KEY_ENTER ? 40 : 0;
        if (usb)
            ui_editor_key(e, usb, 1, 0);
    }
}
ui_editor *ui_editor_create(lv_obj_t *parent, TextBuffer *buffer) {
    ui_editor *e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    e->wrap = 1;
    e->goal = -1;
    e->root = lv_obj_create(parent);
    lv_obj_remove_style_all(e->root);
    lv_obj_set_style_bg_color(e->root, ui_colour(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(e->root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(e->root, ui_colour(UI_BORDER), 0);
    lv_obj_set_style_border_width(e->root, gx_lv_px(1), 0);
    lv_obj_set_style_border_color(e->root, ui_colour(UI_ACCENT), LV_STATE_FOCUSED);
    lv_obj_set_style_pad_all(e->root, gx_lv_px(8), 0);
    lv_obj_set_scroll_dir(e->root, LV_DIR_VER);
    lv_obj_set_scroll_elastic(e->root, false);
    lv_obj_set_scroll_momentum(e->root, false);
    lv_obj_set_scroll_on_focus(e->root, false);
    lv_obj_set_scroll_chain(e->root, false);
    lv_obj_set_click_focusable(e->root, true);
    lv_group_add_obj(gx_lv_group(), e->root);
    e->extent = lv_obj_create(e->root);
    lv_obj_remove_style_all(e->extent);
    lv_obj_set_clickable(e->extent, false);
    lv_obj_set_scrollable(e->extent, false);
    lv_obj_set_pos(e->extent, 0, 0);
    e->work = lv_timer_create(work, 16, e);
    lv_obj_add_event_cb(e->root, event, LV_EVENT_ALL, e);
    metrics(e);
    ui_editor_buffer(e, buffer);
    return e;
}
lv_obj_t *ui_editor_object(ui_editor *e) { return e ? e->root : NULL; }
void ui_editor_buffer(ui_editor *e, TextBuffer *buffer) {
    unsigned long bounds[2];
    if (!e)
        return;
    e->buffer = buffer;
    memset(e->syntax, 0, sizeof(e->syntax));
    e->cursor = e->anchor = 0;
    e->affinity = 0;
    e->goal = -1;
    e->dragging = 0;
    lv_obj_set_scrollable(e->root, true);
    e->count = 0;
    e->reveal = 1;
    e->complete = !buffer;
    paint_clear(e);
    lv_obj_scroll_to(e->root, 0, 0, LV_ANIM_OFF);
    if (buffer) {
        text_damage(buffer, bounds);
        restart(e, 0);
        step_layout(e, EDITOR_WORK);
    } else {
        lv_timer_pause(e->work);
        lv_obj_set_size(e->extent, 0, 0);
    }
    lv_obj_invalidate(e->root);
}
void ui_editor_wrap(ui_editor *e, int enabled) {
    if (!e || e->wrap == !!enabled)
        return;
    e->wrap = !!enabled;
    lv_obj_set_scroll_dir(e->root, enabled ? LV_DIR_VER : LV_DIR_ALL);
    if (e->buffer) {
        restart(e, 0);
        notify(e, 1);
    }
}
int ui_editor_get_wrap(ui_editor *e) { return e && e->wrap; }
void ui_editor_density(ui_editor *e) {
    if (!e)
        return;
    lv_obj_set_style_border_width(e->root, gx_lv_px(1), 0);
    lv_obj_set_style_pad_all(e->root, gx_lv_px(8), 0);
    if (e->buffer) {
        editor_layout(e);
        notify(e, 1);
    }
}
void ui_editor_focus(ui_editor *e) { if (e) lv_group_focus_obj(e->root); }
int ui_editor_focused(ui_editor *e) {
    return e && gx_lv_active() && lv_group_get_focused(gx_lv_group()) == e->root;
}
int ui_editor_ready(ui_editor *e) { return e && e->complete; }
unsigned long ui_editor_display_rows(ui_editor *e) { return e ? e->count : 0; }
const char *ui_editor_error(ui_editor *e) {
    if (!e)
        return "Text viewport unavailable.";
    if (e->error)
        return e->error;
    return e->buffer ? text_error(e->buffer) : "";
}
