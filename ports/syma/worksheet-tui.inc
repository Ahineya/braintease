/* Full-screen worksheet. Storage/evaluation is shared with --line and --run. */
#include <errno.h>
#include "worksheet-pretty.inc"

enum {
    UI_BODY,
    UI_HEADER,
    UI_SIDE,
    UI_SELECTED,
    UI_MUTED,
    UI_ACCENT,
    UI_RESULT,
    UI_WARNING,
    UI_ERROR,
    UI_COMMENT,
    UI_KEYWORD,
    UI_STRING,
    UI_NUMBER,
    UI_OPERATOR,
    UI_TYPE,
    UI_ROLES
};
static struct {
    unsigned active, raw, focus, mode, changed, paste, append, note, busy, warning, selection,
        repaint;
    Count selected, outline, cached, running, target;
    unsigned long top, cursor, editor_top, source_rows, result_rows;
    int width, height, side, cache_width, cache_height, goal;
    TextBuffer *source, *result, *editor;
    chtype colors[UI_ROLES];
    char message[256], answer[1024];
} ui;
static void ui_message(const char *message, unsigned warning) {
    strncpy(ui.message, message, sizeof(ui.message) - 1);
    ui.message[sizeof(ui.message) - 1] = 0;
    ui.warning = warning;
    ui.repaint = 1;
}
static void ui_fill(int y, int x, int width, unsigned role) {
    chtype cells[512];
    int i;
    if (y < 0 || y >= LINES || x < 0 || x >= COLS || width <= 0)
        return;
    if (width > COLS - x)
        width = COLS - x;
    for (i = 0; i < width; i++)
        cells[i] = ' ' | ui.colors[role];
    ripple_curses_cells(stdscr, y, x, cells, width);
}
/* All labels pass through the Unicode cell renderer; control bytes never reach stdout. */
static void ui_label(int y, int x, int width, const char *text, unsigned role) {
    chtype cells[512];
    size_t at = 0, n = strlen(text);
    int used = 0;
    uint32_t cp, glyph;
    struct ru_break cluster = {0};
    int last = -1, glyph_width;
    if (y < 0 || y >= LINES || x < 0 || x >= COLS || width < 1)
        return;
    if (width > COLS - x)
        width = COLS - x;
    while (at < n) {
        cp = ru_next((const unsigned char *)text, n, &at);
        if (cp < 32 || (cp >= 127 && cp < 160))
            cp = ' ';
        if (ru_join(&cluster, cp) && last >= 0) {
            glyph = ru_append(cells[last] & A_CHARTEXT, cp);
            cells[last] = glyph | ui.colors[role];
            continue;
        }
        glyph_width = ru_width(cp);
        if (glyph_width < 1) {
            cp = RU_REPLACEMENT;
            glyph_width = 1;
        }
        if (used + glyph_width > width)
            break;
        last = used;
        cells[used++] = cp | ui.colors[role];
        if (glyph_width == 2)
            cells[used++] = RU_CONTINUATION | ui.colors[role];
    }
    while (used < width)
        cells[used++] = ' ' | ui.colors[role];
    ripple_curses_cells(stdscr, y, x, cells, width);
}
static void ui_preview(int y, int x, int width, const char *text, unsigned role) {
    size_t n = 0, cut;
    while (text[n] && text[n] != '\n' && text[n] != '\r')
        n++;
    cut = ru_prefix((const unsigned char *)text, n, width > 3 ? width - 3 : width);
    char *shortened = syma_mem(((Count)cut + 5) * sizeof(char));
    memcpy(shortened, text, cut * sizeof(char));
    shortened[cut] = 0;
    if (text[cut] && width > 3)
        strcat(shortened, "...");
    ui_label(y, x, width, shortened, role);
    free(shortened);
}
static int ui_event(uint32_t *value) {
    int result;
    errno = 0;
    result = wget_wch(stdscr, value);
#ifdef RIPPLE_CURSES_HOST
    extern int ripple_curses_input_closed(void);
    if (result == ERR && ripple_curses_input_closed())
        return -2;
#endif
    if (result == ERR && (feof(stdin) || errno == EIO || errno == EBADF))
        return -2;
    return result;
}
static unsigned long ui_rows(TextBuffer *b, unsigned width) {
    unsigned long i = 0, info[4];
    int status;
    while ((status = text_wrap_info(b, i, width, 4, info)) > 0)
        i++;
    if (status < 0)
        syma_fail(text_error(b));
    return i;
}
static TextBuffer *ui_buffer(const char *source, unsigned language) {
    TextBuffer *b = text_new();
    if (!b)
        syma_fail("cannot allocate text buffer");
    /* Keep the document intact even when its source exceeds the editor's limit. */
    if (strlen(source) > TEXT_MAX)
        source = "This cell exceeds the 512 KiB screen editor limit.\n"
                 "Its full contents are preserved. Use --line mode or an external editor.";
    if (text_append(b, (const unsigned char *)source, (Count)strlen(source)) < 0) {
        text_free(b);
        syma_fail("cannot load cell into the text viewer");
    }
    text_language(b, language);
    text_saved(b);
    return b;
}
static void ui_invalidate(void) { ui.cached = (Count)-1; }
static void ui_details(void) {
    int width = ui.width - ui.side - 3;
    if (width < 2)
        return;
    if (ui.cached == ui.selected && ui.cache_width == width && ui.cache_height == ui.height - 6)
        return;
    if (ui.cached == ui.selected && sheet.n && ui.source && ui.result) {
        Cell *c = &sheet.cells[ui.selected];
        unsigned long info[4], anchor = 0;
        unsigned section = 0;
        unsigned long old_total =
            c->note ? ui.source_rows + 1 : ui.source_rows + ui.result_rows + 3;
        unsigned bottom = ui.top && ui.top + ui.cache_height >= old_total;
        if (!bottom && ui.top && ui.top <= ui.source_rows &&
            text_wrap_info(ui.source, ui.top - 1, ui.cache_width, 4, info) > 0) {
            anchor = info[3];
            section = 1;
        } else if (!bottom && !c->note && ui.top > ui.source_rows + 2 &&
                   text_wrap_info(ui.result, ui.top - ui.source_rows - 3, ui.cache_width, 4, info) >
                       0) {
            anchor = info[3];
            section = 2;
        }
        ui.source_rows = ui_rows(ui.source, width);
        ui.result_rows = ui_rows(ui.result, width);
        if (bottom)
            ui.top = (unsigned long)-1;
        else if (section == 1)
            ui.top = 1 + text_wrap_at(ui.source, anchor, width, 4);
        else if (section == 2)
            ui.top = ui.source_rows + 3 + text_wrap_at(ui.result, anchor, width, 4);
        ui.cache_width = width;
        ui.cache_height = ui.height - 6;
        return;
    }
    text_free(ui.source);
    text_free(ui.result);
    ui.source = ui.result = NULL;
    if (!sheet.n)
        return;
    Cell *c = &sheet.cells[ui.selected];
    ui.source = ui_buffer(c->source, c->note ? 0 : 15);
    char *formatted = NULL;
    const char *output = c->output && *c->output ? c->output : "Run this cell to see a result.";
    if (!ui.raw && c->state != 2 && c->output && *c->output)
        output = formatted = display_result(c->output);
    ui.result = ui_buffer(output, ui.raw && c->state != 2 ? 15 : 0);
    free(formatted);
    ui.source_rows = ui_rows(ui.source, width);
    ui.result_rows = ui_rows(ui.result, width);
    ui.cached = ui.selected;
    ui.cache_width = width;
    ui.cache_height = ui.height - 6;
}
static void ui_text_row(TextBuffer *b, unsigned long index, int y, int x, unsigned width,
                        unsigned base, unsigned syntax) {
    unsigned long info[4], styles[7], cells[512];
    unsigned i;
    if (text_wrap_info(b, index, width, 4, info) <= 0)
        return;
    styles[0] = ui.colors[base];
    for (i = 1; i < 7; i++)
        styles[i] = ui.colors[syntax ? UI_COMMENT + i - 1 : base];
    unsigned n = (unsigned)info[2];
    if (n > width)
        n = width;
    if (n && text_render(b, info[0], info[1], n, 4, styles, ui.colors[UI_SELECTED], 0,
                         b == ui.editor && ui.selection ? text_length(b) : 0, cells) < 0)
        syma_fail(text_error(b));
    if (n)
        ripple_curses_cells(stdscr, y, x, cells, n);
}
static void ui_geometry(void) {
    ui.width = COLS;
    ui.height = LINES;
    ui.side = COLS >= 110 ? 32 : COLS >= 80 ? 25 : 21;
}
static void ui_outline(void) {
    int rows = (ui.height - 6) / 2, y = 3;
    Count i;
    char title[100];
    if (rows < 1)
        return;
    if (ui.selected < ui.outline)
        ui.outline = ui.selected;
    if (ui.selected >= ui.outline + (Count)rows)
        ui.outline = ui.selected - rows + 1;
    ui_label(2, 1, ui.side - 1, "CELLS", ui.focus ? UI_MUTED : UI_ACCENT);
    for (i = ui.outline; i < sheet.n && i < ui.outline + (Count)rows; i++) {
        Cell *c = &sheet.cells[i];
        unsigned role = i == ui.selected ? UI_SELECTED : UI_SIDE;
        snprintf(title, sizeof(title), "%lu %s  %s", (unsigned long)i + 1,
                 c->note ? "Note" : "Code", c->note ? "" : state(c));
        ui_label(y++, 0, ui.side, title, role);
        const char *p = c->source;
        while (isspace((unsigned char)*p))
            p++;
        ui_preview(y++, 2, ui.side - 2, *p ? p : "(empty cell)", role);
    }
    while (y < ui.height - 3)
        ui_fill(y++, 0, ui.side, UI_SIDE);
    for (y = 2; y < ui.height - 3; y++)
        ui_label(y, ui.side, 1, "|", UI_MUTED);
}
static void ui_detail(void) {
    int x = ui.side + 2, width = ui.width - x - 1, y;
    unsigned long row, source_end, result_begin, total;
    char title[120];
    if (!sheet.n) {
        ui_label(4, x, width, "A worksheet for objects, algebra and ideas.", UI_ACCENT);
        ui_label(6, x, width, "Press A to add code, or T to add a note.", UI_BODY);
        return;
    }
    ui_details();
    Cell *c = &sheet.cells[ui.selected];
    snprintf(title, sizeof(title), "Cell %lu / %lu  |  %s  |  %s", (unsigned long)ui.selected + 1,
             (unsigned long)sheet.n, c->note ? "Note" : "Code", c->note ? "" : state(c));
    ui_label(2, x, width, title, ui.focus ? UI_ACCENT : UI_MUTED);
    source_end = ui.source_rows + 1;
    result_begin = source_end + 1;
    total = c->note ? source_end : result_begin + 1 + ui.result_rows;
    unsigned long height = (unsigned)(ui.height - 6);
    if (ui.top > (total > height ? total - height : 0))
        ui.top = total > height ? total - height : 0;
    for (y = 3, row = ui.top; y < ui.height - 3 && row < total; y++, row++) {
        if (!row)
            ui_label(y, x, width, c->note ? "Note" : "Source", UI_ACCENT);
        else if (row < source_end)
            ui_text_row(ui.source, row - 1, y, x, width, UI_BODY, !c->note);
        else if (!c->note && row == result_begin)
            ui_label(y, x, width,
                     c->state == 2                              ? "Error"
                     : c->state == 0 && c->output && *c->output ? "Previous result (needs update)"
                     : ui.raw                                   ? "Result  [raw]"
                                                                : "Result",
                     c->state == 2   ? UI_ERROR
                     : c->state == 0 ? UI_WARNING
                                     : UI_RESULT);
        else if (!c->note && row > result_begin)
            ui_text_row(ui.result, row - result_begin - 1, y, x, width,
                        c->state == 2 ? UI_ERROR : UI_RESULT, ui.raw && c->state != 2);
    }
    snprintf(title, sizeof(title), "View row %lu / %lu  |  %s", (unsigned long)ui.top + 1,
             (unsigned long)total, ui.raw ? "Raw result" : "Readable result");
    ui_label(ui.height - 3, x, width, title, UI_MUTED);
}
static void ui_draw(void) {
    int y;
    char heading[160];
    ui_geometry();
    wbkgd(stdscr, ui.colors[UI_BODY]);
    const char *path = sheet.path ? strrchr(sheet.path, '/') : NULL;
    path = path ? path + 1 : sheet.path ? sheet.path : "Untitled";
    snprintf(heading, sizeof(heading), " Syma  |  %.100s%s", path,
             sheet.dirty ? "  * Unsaved" : "");
    ui_label(0, 0, COLS, heading, UI_HEADER);
    if (COLS < 60 || LINES < 14) {
        ui_label(3, 0, COLS, "Use a terminal of at least 60 x 14.", UI_WARNING);
        ui_label(5, 0, COLS, "Resize to continue. Q closes the worksheet.", UI_BODY);
    } else {
        ui_label(1, 0, COLS,
                 " F1 Help F2 Save F3 Open F4 Save as F5 Run all F6 Run cell F7 Raw F8 Limit",
                 UI_SIDE);
        for (y = 2; y < LINES - 2; y++)
            ui_fill(y, 0, ui.side, UI_SIDE);
        ui_outline();
        ui_detail();
        ui_label(LINES - 1, 0, COLS,
                 " Up/Down Cell  PgUp/Dn Scroll  Enter Edit  A/T Add  D Remove  Q Quit", UI_SIDE);
    }
    if (ui.busy) {
        snprintf(heading, sizeof(heading), " Running cell %lu of %lu  |  Esc stops between cells",
                 (unsigned long)ui.running + 1, (unsigned long)ui.target);
        ui_label(LINES - 2, 0, COLS, heading, UI_WARNING);
    } else
        ui_label(LINES - 2, 0, COLS, ui.message, ui.warning ? UI_ERROR : UI_MUTED);
    curs_set(0);
    wrefresh(stdscr);
}
static void ui_choose(Count index) {
    if (!sheet.n)
        return;
    if (index >= sheet.n)
        index = sheet.n - 1;
    ui.selected = index;
    ui.top = 0;
    ui_invalidate();
}
static int ui_progress(Count cell, Count total) {
    uint32_t key;
    int status;
    wtimeout(stdscr, 0);
    while ((status = ui_event(&key)) >= 0) {
        if ((status == OK && (key == 27 || key == 3)) || status == -2) {
            wtimeout(stdscr, 100);
            return 0;
        }
    }
    wtimeout(stdscr, 100);
    if (status == -2)
        return 0;
    ui.running = cell;
    ui.target = total;
    ui_draw();
    return 1;
}
static void ui_run(unsigned all) {
    char message[120];
    if (!sheet.n) {
        ui_message("Add a code cell first.", 0);
        return;
    }
    if (!all && sheet.cells[ui.selected].note) {
        ui_message("Select a code cell to run.", 0);
        return;
    }
    ui.busy = 1;
    ui.running = ui.selected;
    ui.target = sheet.n;
    int status = all                                 ? run(sheet.n)
                 : ui_progress(ui.selected, sheet.n) ? run_cell(ui.selected)
                                                     : -1;
    ui.busy = 0;
    ui_invalidate();
    if (status < 0)
        ui_message("Run stopped. Remaining results need updating.", 0);
    else if (!status)
        ui_message("A cell has an error. Edit it and run again.", 1);
    else {
        snprintf(message, sizeof(message),
                 all ? "Worksheet evaluated. F7 switches result display."
                     : "Cell %lu evaluated. F7 switches result display.",
                 (unsigned long)ui.selected + 1);
        ui_message(message, 0);
    }
}
/* Prompts are kept in screen, with UTF-8 editing and a horizontally scrolling path. */
static char *ui_prompt(const char *label, const char *initial) {
    size_t n = initial ? strlen(initial) : 0;
    uint32_t key;
    int status;
    if (n >= sizeof(ui.answer))
        n = sizeof(ui.answer) - 1;
    memcpy(ui.answer, initial ? initial : "", n);
    ui.answer[n] = 0;
    for (;;) {
        ui_draw();
        if (COLS < 10 || LINES < 4) {
            status = ui_event(&key);
            if (status == -2 || (status == OK && key == 27))
                return NULL;
            continue;
        }
        ui_label(LINES - 3, 0, COLS, label, UI_ACCENT);
        size_t start = 0;
        while (ru_columns((const unsigned char *)ui.answer + start, n - start) >=
               (size_t)(COLS - 2))
            start = ru_next_grapheme((const unsigned char *)ui.answer, n, start, NULL);
        ui_label(LINES - 2, 0, COLS, ui.answer + start, UI_SELECTED);
        ui_label(LINES - 1, 0, COLS, " Enter Accept   Esc Cancel   Ctrl-U Clear", UI_SIDE);
        wmove(stdscr, LINES - 2,
              (int)ru_columns((const unsigned char *)ui.answer + start, n - start));
        curs_set(1);
        wrefresh(stdscr);
        status = ui_event(&key);
        if (status == -2)
            return NULL;
        if (status == ERR)
            continue;
        if (status == OK && key == 27)
            return NULL;
        if (status == OK && (key == '\n' || key == '\r'))
            return n ? ui.answer : NULL;
        if ((status == KEY_CODE_YES && key == KEY_BACKSPACE) ||
            (status == OK && (key == 8 || key == 127))) {
            n = ru_previous_grapheme((const unsigned char *)ui.answer, n, n);
            ui.answer[n] = 0;
        } else if (status == OK && key == 21) {
            n = 0;
            ui.answer[0] = 0;
        } else if (status == OK && key >= 32 && key != 127) {
            unsigned char bytes[4];
            unsigned count = ru_encode(key, bytes);
            if (n + count < sizeof(ui.answer)) {
                memcpy(ui.answer + n, bytes, count);
                n += count;
                ui.answer[n] = 0;
            }
        }
    }
}
static int ui_question(const char *question, const char *keys) {
    uint32_t key;
    int status;
    for (;;) {
        ui_draw();
        ui_label(LINES - 3, 0, COLS, question, UI_ACCENT);
        ui_label(LINES - 2, 0, COLS, keys, UI_SELECTED);
        wrefresh(stdscr);
        status = ui_event(&key);
        if (status == -2 || (status == OK && (key == 27 || key == 3)))
            return 27;
        if (status == OK && key < 128 && strchr("yYnNsSdD", (int)key))
            return tolower((int)key);
    }
}
static int ui_save(unsigned as) {
    char initial[1024];
    if (as || !sheet.path) {
        const char *home = getenv("HOME");
        snprintf(initial, sizeof(initial), "%s/worksheet.symws", home && *home ? home : ".");
        char *path = ui_prompt("Save worksheet as", sheet.path ? sheet.path : initial);
        if (!path) {
            ui_message("Save cancelled.", 0);
            return 0;
        }
        if (!sheet.path || strcmp(path, sheet.path)) {
            FILE *existing = fopen(path, "r");
            if (existing) {
                fclose(existing);
                if (ui_question("Replace this existing file?", " Y Replace   N Cancel") != 'y')
                    return 0;
            }
        }
        save(path);
    } else
        save(sheet.path);
    ui_message("Worksheet saved.", 0);
    return 1;
}
static int ui_leave(void) {
    if (!sheet.dirty)
        return 1;
    int answer = ui_question("Save worksheet changes before continuing?",
                             " S Save   D Discard   Esc Cancel");
    return answer == 'd' || (answer == 's' && ui_save(0));
}
static void ui_release_editor(void) {
    text_free(ui.editor);
    ui.editor = NULL;
    ui.mode = ui.paste = ui.changed = ui.selection = 0;
}
static void ui_edit(unsigned append, unsigned note) {
    if (!append && !sheet.n)
        return;
    if (append && sheet.n == CELL_LIMIT) {
        ui_message("Worksheet has reached 512 cells.", 1);
        return;
    }
    const char *source = append ? "" : sheet.cells[ui.selected].source;
    if (strlen(source) > TEXT_MAX) {
        ui_message("Cell exceeds the screen editor's 512 KiB limit; source is preserved.", 1);
        return;
    }
    ui.editor = ui_buffer(source, (append ? note : sheet.cells[ui.selected].note) ? 0 : 15);
    ui.append = append;
    ui.note = append ? note : sheet.cells[ui.selected].note;
    ui.mode = 1;
    ui.cursor = ui.editor_top = 0;
    ui.goal = -1;
    ui.paste = ui.selection = 0;
    ui_message("Edit cell. F2 applies changes; Esc returns to the worksheet.", 0);
}
static void ui_commit(void) {
    unsigned long n = text_length(ui.editor);
    char *source = syma_mem((n + 1) * sizeof(char));
    text_copy(ui.editor, 0, n, (unsigned char *)source);
    source[n] = 0;
    Count index = ui.selected;
    if (ui.append) {
        index = sheet.n ? ui.selected + 1 : 0;
        memmove(sheet.cells + index + 1, sheet.cells + index, (sheet.n - index) * sizeof(Cell));
        memset(sheet.cells + index, 0, sizeof(Cell));
        sheet.cells[index].note = ui.note;
        sheet.n++;
    }
    free(sheet.cells[index].source);
    sheet.cells[index].source = source;
    invalidate(index);
    ui_release_editor();
    ui_choose(index);
    ui_message("Cell updated. Its result and later results need updating.", 0);
}
static void ui_editor_draw(void) {
    int y, width;
    unsigned long info[4], row;
    char title[140];
    ui_geometry();
    width = COLS - 4;
    if (COLS < 60 || LINES < 14) {
        ui_draw();
        return;
    }
    long caret_row = text_wrap_at(ui.editor, ui.cursor, width, 4);
    if (caret_row < 0)
        syma_fail(text_error(ui.editor));
    unsigned long height = (unsigned)(LINES - 5);
    if ((unsigned long)caret_row < ui.editor_top)
        ui.editor_top = caret_row;
    if ((unsigned long)caret_row >= ui.editor_top + height)
        ui.editor_top = caret_row - height + 1;
    wbkgd(stdscr, ui.colors[UI_BODY]);
    snprintf(title, sizeof(title), " Syma  |  %s %s cell%s", ui.append ? "New" : "Editing",
             ui.note ? "note" : "code", text_modified(ui.editor) ? "  *" : "");
    ui_label(0, 0, COLS, title, UI_HEADER);
    ui_label(1, 0, COLS, " F2 Apply  Ctrl-S Save  F5 Apply+run  Esc Cancel  Ctrl-Z/Y Undo/redo",
             UI_SIDE);
    for (y = 3, row = ui.editor_top; y < LINES - 2; y++, row++)
        ui_text_row(ui.editor, row, y, 2, width, UI_BODY, !ui.note);
    snprintf(title, sizeof(title), "%s  Line %lu  Column %lu  |  Wrapped to window width",
             ui.paste ? "Pasting..." : "Editing", (unsigned long)text_row(ui.editor, ui.cursor) + 1,
             (unsigned long)text_visual(ui.editor, ui.cursor, 4) + 1);
    ui_label(LINES - 2, 0, COLS, ui.warning ? ui.message : title, ui.warning ? UI_ERROR : UI_MUTED);
    ui_label(LINES - 1, 0, COLS,
             " Arrows Move  Home/End  PgUp/Dn  Enter New line  Tab Indent  F2 Apply", UI_SIDE);
    if (text_wrap_info(ui.editor, caret_row, width, 4, info) > 0) {
        unsigned long col = text_visual(ui.editor, ui.cursor, 4) - info[1];
        if (col >= (unsigned)width)
            col = width - 1;
        wmove(stdscr, 3 + (int)(caret_row - ui.editor_top), 2 + (int)col);
    }
    curs_set(1);
    wrefresh(stdscr);
}
static void ui_vertical(int distance) {
    unsigned long info[4];
    unsigned width = COLS - 4;
    long row = text_wrap_at(ui.editor, ui.cursor, width, 4);
    if (row < 0 || text_wrap_info(ui.editor, row, width, 4, info) <= 0)
        return;
    if (ui.goal < 0)
        ui.goal = (int)(text_visual(ui.editor, ui.cursor, 4) - info[1]);
    long target = row + distance;
    if (target < 0)
        target = 0;
    long at = text_wrap_column(ui.editor, target, ui.goal, width, 4);
    if (at >= 0)
        ui.cursor = at;
    else if (distance > 0)
        ui.cursor = text_length(ui.editor);
}
static void ui_editor_key(int status, uint32_t key) {
    unsigned long before = ui.cursor, end;
    long moved;
    if (status == KEY_CODE_YES && key == KEY_RESIZE)
        return;
    if (status == KEY_CODE_YES && key == KEY_PASTE_BEGIN) {
        ui.paste = 1;
        text_begin(ui.editor, ui.cursor);
        return;
    }
    if (status == KEY_CODE_YES && key == KEY_PASTE_END) {
        text_end(ui.editor, ui.cursor);
        ui.paste = 0;
        return;
    }
    if (!ui.paste && ((status == OK && (key == 27 || key == 3 || key == 17)))) {
        if (!text_modified(ui.editor) ||
            ui_question("Discard edits to this cell?", " Y Discard   N Keep editing") == 'y')
            ui_release_editor();
        return;
    }
    if (!ui.paste && status == KEY_CODE_YES && (key == KEY_F(2) || key == KEY_F(5))) {
        ui_commit();
        if (key == KEY_F(5))
            ui_run(0);
        return;
    }
    if (!ui.paste && status == OK && key == 19) {
        ui_commit();
        ui_save(0);
        return;
    }
    if (!ui.paste && status == KEY_CODE_YES) {
        if (key != KEY_BACKSPACE && key != KEY_DC && key != KEY_ENTER)
            ui.selection = 0;
        if (key == KEY_LEFT)
            ui.cursor = text_previous(ui.editor, ui.cursor);
        else if (key == KEY_RIGHT)
            ui.cursor = text_next(ui.editor, ui.cursor);
        else if (key == KEY_UP || key == KEY_DOWN) {
            ui_vertical(key == KEY_UP ? -1 : 1);
            return;
        } else if (key == KEY_PPAGE || key == KEY_NPAGE) {
            ui_vertical((LINES - 6) * (key == KEY_PPAGE ? -1 : 1));
            return;
        } else if (key == KEY_HOME)
            ui.cursor = text_start(ui.editor, text_row(ui.editor, ui.cursor));
        else if (key == KEY_END)
            ui.cursor = text_endline(ui.editor, text_row(ui.editor, ui.cursor));
        else if (key != KEY_BACKSPACE && key != KEY_DC && key != KEY_ENTER)
            return;
        if (key != KEY_BACKSPACE && key != KEY_DC && key != KEY_ENTER) {
            ui.goal = -1;
            return;
        }
    }
    ui.goal = -1;
    if (!ui.paste && status == OK && key == 1) {
        ui.selection = 1;
        ui.cursor = text_length(ui.editor);
        return;
    }
    if (!ui.paste && status == OK && (key == 26 || key == 25)) {
        ui.selection = 0;
        moved = key == 26 ? text_undo(ui.editor) : text_redo(ui.editor);
        if (moved >= 0)
            ui.cursor = moved;
        return;
    }
    if (!ui.paste && status == OK && (key == 5)) {
        ui.selection = 0;
        ui.cursor = text_endline(ui.editor, text_row(ui.editor, ui.cursor));
        return;
    }
    if (status == OK && !key) {
        ui_message("NUL is not supported in cell text.", 1);
        return;
    }
    if (!ui.paste && status == OK && key < 32 && key != 8 && key != 9 && key != 10 && key != 13)
        return;
    if (!ui.paste)
        text_begin(ui.editor, before);
    int result = 0;
    if (ui.selection) {
        if (text_replace(ui.editor, 0, text_length(ui.editor), NULL, 0) < 0) {
            ui_message(text_error(ui.editor), 1);
            return;
        }
        ui.cursor = 0;
        ui.selection = 0;
    }
    if (!ui.paste && ((status == KEY_CODE_YES && key == KEY_BACKSPACE) ||
                      (status == OK && (key == 8 || key == 127)))) {
        unsigned long previous = text_previous(ui.editor, ui.cursor);
        result = text_replace(ui.editor, previous, ui.cursor - previous, NULL, 0);
        if (!result)
            ui.cursor = previous;
    } else if (!ui.paste && status == KEY_CODE_YES && key == KEY_DC) {
        end = text_next(ui.editor, ui.cursor);
        result = text_replace(ui.editor, ui.cursor, end - ui.cursor, NULL, 0);
    } else if (!ui.paste && ((status == OK && (key == '\r' || key == '\n')) ||
                             (status == KEY_CODE_YES && key == KEY_ENTER))) {
        moved = text_newline(ui.editor, ui.cursor, !ui.note);
        if (moved < 0)
            result = -1;
        else
            ui.cursor = moved;
    } else if (!ui.paste && status == OK && key == '\t') {
        result = text_replace(ui.editor, ui.cursor, 0, (const unsigned char *)"    ", 4);
        if (!result)
            ui.cursor += 4;
    } else if (status == OK && key && (ui.paste || (key >= 32 && key != 127))) {
        unsigned char bytes[4096];
        unsigned n = ru_encode(key, bytes);
        n += ripple_curses_text_run(stdscr, bytes + n, sizeof(bytes) - n, ui.paste);
        result = text_replace(ui.editor, ui.cursor, 0, bytes, n);
        if (!result)
            ui.cursor += n;
    }
    if (!ui.paste)
        text_end(ui.editor, ui.cursor);
    if (result < 0)
        ui_message(text_error(ui.editor), 1);
    else
        ui.warning = 0;
}
static const char *ui_help[] = {
    "SYMA WORKSHEET",
    "",
    "Browse cells on the left; read complete, wrapped source and results on the right.",
    "Not run: no result yet. Needs update: an earlier edit or a reopened cache needs a run.",
    "Ready: evaluated in this session. Error: open the cell and correct its source.",
    "",
    "F5             Run the whole worksheet",
    "F6             Run only the selected cell in the live session",
    "Enter / E      Edit selected cell; F2 applies edits, Esc cancels",
    "A / T          Add code / add note after the selected cell",
    "D / Delete     Remove a cell (asks first)",
    "[ / ]          Move selected cell up / down",
    "Up / Down      Select cells; when reading the detail pane, scroll it",
    "Tab / Left / Right   Switch between the cell list and reading pane",
    "PgUp / PgDn    Scroll source and result; mouse wheel also scrolls",
    "F7             Toggle readable mathematics / exact raw result",
    "F2 / Ctrl-S    Save   F4: Save as   F3: Open   Q / Ctrl-Q: Close",
    "F8             Change the saved evaluation limit",
    "",
    "EDITING",
    "Arrow keys and Home/End move the cursor. Long lines wrap without changing source.",
    "Ctrl-A selects all; Ctrl-Z / Ctrl-Y undo and redo. Paste is inserted in batches as one undo "
    "group.",
    "Each code cell contains one complete term. Define stores a named result:",
    "  {Define $polynomial {Sum {Power x 3} {Product 2 x}}}",
    "  {Differentiate $polynomial x}",
    "",
    "F5 rebuilds definitions from the beginning. Esc stops between cells.",
    "F6 keeps session definitions. Run imports/definitions first, or use F5.",
    "After removing definitions, use F5 to rebuild a clean session.",
    "Saving keeps cells, cached results and the evaluation limit. Cached results need a run after "
    "reopening.",
    "Use syma worksheet FILE --line for colon commands or FILE --run for scripts.",
    "",
    "Press Esc or F1 to return. PgUp/PgDn scroll this help."};
static unsigned ui_help_top;
static void ui_help_draw(void) {
    int y;
    unsigned line;
    wbkgd(stdscr, ui.colors[UI_BODY]);
    ui_label(0, 0, COLS, " Syma  |  Help", UI_HEADER);
    TextBuffer *b = text_new();
    if (!b)
        return;
    for (line = 0; line < sizeof(ui_help) / sizeof(*ui_help); line++) {
        text_append(b, (const unsigned char *)ui_help[line], strlen(ui_help[line]));
        text_append(b, (const unsigned char *)"\n", 1);
    }
    for (y = 2; y < LINES - 1; y++)
        ui_text_row(b, ui_help_top + y - 2, y, 2, COLS - 4, UI_BODY, 0);
    text_free(b);
    ui_label(LINES - 1, 0, COLS, " Esc Return   Up/Down or PgUp/PgDn Scroll", UI_SIDE);
    curs_set(0);
    wrefresh(stdscr);
}
static void ui_colors(void) {
    static const int fg[] = {252, 231, 250, 231, 245, 81,  158, 214,
                             203, 245, 81,  151, 215, 117, 147};
    static const int bg[] = {235, 24,  236, 31,  235, 235, 235, 235,
                             235, 235, 235, 235, 235, 235, 235};
    unsigned i;
    int colored = start_color() == OK;
    for (i = 0; i < UI_ROLES; i++) {
        ui.colors[i] = i == UI_HEADER || i == UI_ACCENT ? A_BOLD : 0;
        if (colored && init_pair(i + 1, fg[i], bg[i]) == OK)
            ui.colors[i] |= COLOR_PAIR(i + 1);
        else if (i == UI_HEADER || i == UI_SELECTED)
            ui.colors[i] |= A_REVERSE;
    }
}
static void ui_browser_key(int status, uint32_t key) {
    if ((status == OK && (key == 'q' || key == 'Q' || key == 17 || key == 3))) {
        if (ui_leave())
            ui.active = 0;
    } else if (status == KEY_CODE_YES && key == KEY_F(1)) {
        ui.mode = 2;
        ui_help_top = 0;
    } else if ((status == KEY_CODE_YES && key == KEY_F(2)) || (status == OK && key == 19))
        ui_save(0);
    else if (status == KEY_CODE_YES && key == KEY_F(4))
        ui_save(1);
    else if (status == KEY_CODE_YES && key == KEY_F(8)) {
        char value[32];
        snprintf(value, sizeof(value), "%lu", (unsigned long)sheet.limit);
        char *answer = ui_prompt("Evaluation limit (1 to 10000000)", value);
        if (answer) {
            char *end;
            unsigned long limit = strtoul(answer, &end, 10);
            if (*end || limit < 1 || limit > 10000000UL)
                ui_message("Enter a limit between 1 and 10000000.", 1);
            else {
                sheet.limit = limit;
                invalidate(0);
                ui_invalidate();
                ui_message("Evaluation limit updated.", 0);
            }
        }
    } else if (status == KEY_CODE_YES && key == KEY_F(3)) {
        if (ui_leave()) {
            char *path = ui_prompt("Open worksheet", sheet.path ? sheet.path : "");
            if (path) {
                load(path);
                ui_choose(0);
                ui_message("Worksheet opened. Cached results need a new run.", 0);
            }
        }
    } else if (status == KEY_CODE_YES && key == KEY_F(5))
        ui_run(1);
    else if (status == KEY_CODE_YES && key == KEY_F(6))
        ui_run(0);
    else if (status == KEY_CODE_YES && key == KEY_F(7)) {
        ui.raw = !ui.raw;
        ui_invalidate();
        ui.top = 0;
    } else if (status == OK && (key == '\t'))
        ui.focus = !ui.focus;
    else if (status == KEY_CODE_YES && (key == KEY_LEFT || key == KEY_RIGHT))
        ui.focus = key == KEY_RIGHT;
    else if (status == KEY_CODE_YES && (key == KEY_NPAGE || key == KEY_PPAGE)) {
        unsigned long amount = LINES > 8 ? LINES - 8 : 1;
        ui.top = key == KEY_NPAGE ? ui.top + amount : ui.top > amount ? ui.top - amount : 0;
    } else if (status == KEY_CODE_YES && (key == KEY_DOWN || key == KEY_UP)) {
        if (ui.focus) {
            if (key == KEY_DOWN)
                ui.top++;
            else if (ui.top)
                ui.top--;
        } else if (key == KEY_DOWN)
            ui_choose(ui.selected + 1);
        else if (ui.selected)
            ui_choose(ui.selected - 1);
    } else if (status == KEY_CODE_YES && (key == KEY_HOME || key == KEY_END)) {
        if (ui.focus)
            ui.top = key == KEY_HOME ? 0 : (unsigned long)-1;
        else
            ui_choose(key == KEY_HOME ? 0 : sheet.n ? sheet.n - 1 : 0);
    } else if (status == KEY_CODE_YES && key == KEY_MOUSE) {
        MEVENT mouse;
        if (getmouse(&mouse) == OK) {
            if (mouse.bstate & BUTTON4_PRESSED)
                ui.top = ui.top > 3 ? ui.top - 3 : 0;
            else if (mouse.bstate & BUTTON5_PRESSED)
                ui.top += 3;
            else if ((mouse.bstate & BUTTON1_PRESSED) && mouse.x < ui.side && mouse.y >= 3) {
                Count index = ui.outline + (mouse.y - 3) / 2;
                if (index < sheet.n) {
                    ui_choose(index);
                    ui.focus = 0;
                }
            } else if (mouse.bstate & BUTTON1_PRESSED)
                ui.focus = 1;
        }
    } else if ((status == OK && (key == '\n' || key == '\r' || key == 'e')) ||
               (status == KEY_CODE_YES && key == KEY_ENTER))
        ui_edit(0, 0);
    else if (status == OK && (key == 'a' || key == 'A' || key == 't' || key == 'T'))
        ui_edit(1, key == 't' || key == 'T');
    else if (sheet.n && ((status == OK && (key == 'd' || key == 'D')) ||
                         (status == KEY_CODE_YES && key == KEY_DC))) {
        if (ui_question("Remove the selected cell?", " Y Remove   N Cancel") == 'y') {
            Count i = ui.selected;
            free(sheet.cells[i].source);
            free(sheet.cells[i].output);
            memmove(sheet.cells + i, sheet.cells + i + 1, (sheet.n - i - 1) * sizeof(Cell));
            memset(sheet.cells + --sheet.n, 0, sizeof(Cell));
            invalidate(i);
            ui_choose(i);
            ui_invalidate();
            ui_message("Cell removed. Later results need updating.", 0);
        }
    } else if (sheet.n && status == OK && (key == '[' || key == ']')) {
        Count i = ui.selected, j = key == '[' ? i ? i - 1 : 0 : i + 1 < sheet.n ? i + 1 : i;
        if (i != j) {
            Cell c = sheet.cells[i];
            sheet.cells[i] = sheet.cells[j];
            sheet.cells[j] = c;
            invalidate(i < j ? i : j);
            ui_choose(j);
        }
    }
}
static int worksheet_tui(void) {
    jmp_buf boundary;
    jmp_buf *previous = syma_error_target;
    uint32_t key;
    int status;
    memset(&ui, 0, sizeof(ui));
    ui.cached = (Count)-1;
#ifdef RIPPLE_CURSES_HOST
    /* The host build uses the same curses code, with its bounded POSIX input adapter. */
    if (!getenv("TERM"))
        setenv("TERM", "ansi", 0);
    if (!getenv("TERMCAP"))
        setenv("TERMCAP",
               "ansi:co#80:li#24:cl=\\E[H\\E[2J:cm=\\E[%i%d;%dH:me=\\E[0m:vi=\\E[?25l:ve=\\E[?25h:",
               0);
#endif
    if (!initscr()) {
        ui_message("cannot open terminal", 1);
        return 1;
    }
    sheet.tui = ui.active = 1;
    raw();
    noecho();
    keypad(stdscr, 1);
    wtimeout(stdscr, 100);
    ripple_curses_bracketed_paste(1);
    ripple_curses_lines(1);
    mousemask(BUTTON1_PRESSED | BUTTON4_PRESSED | BUTTON5_PRESSED, NULL);
    ui_colors();
    ui_message("Select a cell. F5 runs the worksheet; Enter edits the selected cell.", 0);
    syma_error_target = &boundary;
    for (;;) {
        if (setjmp(boundary)) {
            ui.busy = ui.paste = 0;
            ui_message(syma_message, 1);
            free(sheet.filetext);
            sheet.filetext = NULL;
            free(sheet.savedtext);
            sheet.savedtext = NULL;
            free(sheet.temporary_path);
            sheet.temporary_path = NULL;
            ui_invalidate();
            clearok(stdscr, 1);
        }
        if (!ui.active)
            break;
        if (!ui.paste && ui.repaint) {
            ui.repaint = 0;
            if (ui.mode == 1)
                ui_editor_draw();
            else if (ui.mode == 2)
                ui_help_draw();
            else
                ui_draw();
        }
        status = ui_event(&key);
        if (status == -2)
            break;
        if (status == ERR)
            continue;
        ui.repaint = 1;
        if (COLS < 60 || LINES < 14) {
            if (!ui.mode && status == OK && (key == 'q' || key == 'Q' || key == 17) && ui_leave())
                ui.active = 0;
            continue;
        }
        if (ui.mode == 1)
            ui_editor_key(status, key);
        else if (ui.mode == 2) {
            if ((status == OK && (key == 27 || key == 'q')) ||
                (status == KEY_CODE_YES && key == KEY_F(1)))
                ui.mode = 0;
            else if (status == KEY_CODE_YES && (key == KEY_DOWN || key == KEY_NPAGE))
                ui_help_top += key == KEY_DOWN ? 1 : LINES - 6;
            else if (status == KEY_CODE_YES && (key == KEY_UP || key == KEY_PPAGE)) {
                unsigned amount = key == KEY_UP ? 1 : LINES - 6;
                ui_help_top = ui_help_top > amount ? ui_help_top - amount : 0;
            }
        } else
            ui_browser_key(status, key);
        if (!ui.mode)
            syma_gc(0, 0, NULL, NULL, 0);
    }
    syma_error_target = previous;
    ui_release_editor();
    text_free(ui.source);
    text_free(ui.result);
    ripple_curses_bracketed_paste(0);
    mousemask(0, NULL);
    endwin();
    sheet.tui = 0;
    return 0;
}
