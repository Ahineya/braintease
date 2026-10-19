/* SPDX-License-Identifier: MIT */
/* Cache only the requested prefix. Edits invalidate from their logical line;
 * moving and repainting unchanged paragraphs reuse their grapheme layout. */
static int wrap_setup(TextBuffer *b, unsigned width, unsigned tabstop) {
    if (width < 2 || width > 512 || !tabstop || tabstop > 16) return error(b, 2);
    if (width != b->wrap_width || tabstop != b->wrap_tabstop) {
        b->wrap_count = b->wrap_rows = 0; b->wrap_width = width; b->wrap_tabstop = tabstop;
    }
    return 0;
}
static int wrap_add(TextBuffer *b, unsigned long at, unsigned long row, unsigned long left, unsigned long columns) {
    if (b->wrap_count == b->wrap_capacity) {
        unsigned long cap = b->wrap_capacity ? b->wrap_capacity * 2 : 128;
        struct Wrap *rows = realloc(b->wraps, cap * sizeof(*rows));
        if (!rows) return error(b, 1);
        b->wraps = rows; b->wrap_capacity = cap;
    }
    b->wraps[b->wrap_count].at = at; b->wraps[b->wrap_count].row = row;
    b->wraps[b->wrap_count].left = left; b->wraps[b->wrap_count].columns = columns;
    ++b->wrap_count; return 0;
}
static int wrap_line(TextBuffer *b) {
    unsigned long saved_count = b->wrap_count;
    unsigned long row = b->wrap_rows, start = text_start(b, row), end = text_endline(b, row);
    unsigned long p = start, left = 0, column = 0, boundary = start, boundary_column = 0, next;
    unsigned width;
    while (p < end) {
        next = cluster(b, p, end, &width, NULL);
        if (text_byte(b, p) == '\t') width = b->wrap_tabstop - column % b->wrap_tabstop;
        if (column + width - left > b->wrap_width && p > start) {
            if (text_byte(b, p) == ' ' || text_byte(b, p) == '\t') { boundary = p; boundary_column = column; }
            if (boundary > start) { p = boundary; column = boundary_column; }
            if (wrap_add(b, start, row, left, column - left) < 0) goto failed;
            start = p; left = column; boundary = start; continue;
        }
        column += width; p = next;
        if (text_byte(b, p - 1) == ' ' || text_byte(b, p - 1) == '\t') { boundary = p; boundary_column = column; }
    }
    if (wrap_add(b, start, row, left, column - left) < 0) goto failed;
    /* An insertion cursor at an exactly full EOL needs its own visible cell. */
    if (column - left >= b->wrap_width && wrap_add(b, end, row, column, 0) < 0) goto failed;
    ++b->wrap_rows; return 0;
failed:
    b->wrap_count = saved_count; return -1;
}
int text_wrap_info(TextBuffer *b, unsigned long index, unsigned width, unsigned tabstop, unsigned long *info) {
    struct Wrap *w;
    if (!info || wrap_setup(b, width, tabstop) < 0) return -1;
    while (b->wrap_count <= index && b->wrap_rows < b->nlines) if (wrap_line(b) < 0) return -1;
    if (index >= b->wrap_count) return 0;
    w = b->wraps + index; info[0] = w->row; info[1] = w->left; info[2] = w->columns; info[3] = w->at;
    return 1;
}
long text_wrap_at(TextBuffer *b, unsigned long at, unsigned width, unsigned tabstop) {
    unsigned long row = text_row(b, at), lo = 0, hi;
    if (wrap_setup(b, width, tabstop) < 0) return -1;
    while (b->wrap_rows <= row) if (wrap_line(b) < 0) return -1;
    hi = b->wrap_count;
    while (lo + 1 < hi) { unsigned long mid = lo + (hi - lo) / 2; if (b->wraps[mid].at <= at) lo = mid; else hi = mid; }
    return (long)lo;
}
long text_wrap_column(TextBuffer *b, unsigned long index, unsigned long column, unsigned width, unsigned tabstop) {
    unsigned long info[4], p, end, x, next; unsigned cells; int continuation;
    if (text_wrap_info(b, index, width, tabstop, info) != 1) return -1;
    p = info[3]; x = info[1]; end = text_endline(b, info[0]);
    continuation = index + 1 < b->wrap_count && b->wraps[index + 1].row == info[0];
    if (continuation) end = b->wraps[index + 1].at;
    while (p < end) {
        next = cluster(b, p, end, &cells, NULL);
        if (text_byte(b, p) == '\t') cells = tabstop - x % tabstop;
        if (x - info[1] + cells > column || (next == end && continuation)) break;
        x += cells; p = next;
    }
    return (long)p;
}
long text_format(TextBuffer *b, unsigned long cursor, unsigned width, unsigned tabstop) {
    unsigned long row, start, p, next, column, split, finish, end;
    unsigned cells; unsigned char newline[2];
    if (!width) return (long)cursor;
    if (!tabstop || tabstop > 16 || cursor > text_length(b)) return error(b, 2);
    newline[0] = b->crlf ? '\r' : '\n'; newline[1] = '\n';
    for (;;) {
        row = text_row(b, cursor); start = text_start(b, row); end = text_endline(b, row);
        p = start; column = 0; split = finish = start;
        while (p < cursor && p < end) {
            next = cluster(b, p, end, &cells, NULL);
            if (text_byte(b, p) == '\t') cells = tabstop - column % tabstop;
            if (column + cells > width) {
                if (text_byte(b, p) == ' ' || text_byte(b, p) == '\t') { split = p; finish = next; }
                break;
            }
            if (text_byte(b, p) == ' ' || text_byte(b, p) == '\t') {
                if (p == start || (text_byte(b, p - 1) != ' ' && text_byte(b, p - 1) != '\t')) split = p;
                finish = next;
            }
            column += cells; p = next;
        }
        if (p >= cursor || split == start) return (long)cursor;
        while (finish < cursor && (text_byte(b, finish) == ' ' || text_byte(b, finish) == '\t')) ++finish;
        if (text_replace(b, split, finish - split, newline, 1 + b->crlf) < 0) return -1;
        cursor = cursor - (finish - split) + 1 + b->crlf;
    }
}
