/* SPDX-License-Identifier: MIT */
#include "text.h"
#include <stdlib.h>
#include <string.h>
#include "../../runtime/include/ripple_unicode.h"
#define HISTORY_MAX 1048576UL
#define GROUP_MAX 64
struct Edit { unsigned long at, removed, added; unsigned char *old, *newtext; struct Edit *prev, *next; };
struct Group { struct Edit *first, *last; struct Group *next; unsigned long before, after, cursor, end, bytes; };
struct Line { unsigned long at; unsigned state; };
struct Wrap { unsigned long at, row, left, columns; };
struct TextBuffer {
    unsigned char *data;
    unsigned long capacity, lo, hi, nlines, linecap, valid_states, revision, saved, sequence, generation;
    unsigned long cached_states, dirty_first, dirty_until;
    int dirty_layout, overflow;
    struct Line *lines;
    struct Group *undo, *redo;
    unsigned long history_bytes, begin_cursor;
    unsigned groups, language;
    int open_group, error, newline_known, crlf;
    struct Wrap *wraps;
    unsigned long wrap_count, wrap_capacity, wrap_rows;
    unsigned wrap_width, wrap_tabstop;
};
static unsigned long allocated;
static int error(TextBuffer *b, int code) { b->error = code; return -1; }
const char *text_error(TextBuffer *b) {
    static const char *messages[] = {"", "Out of memory", "Invalid text range", "File exceeds 512 KiB or text memory budget", "Binary NUL is not supported", "File exceeds 65536 lines", "Change exceeds undo memory limit", "Text object not found"};
    return b && b->error >= 0 && b->error <= 7 ? messages[b->error] : "Text buffer unavailable";
}
unsigned long text_length(TextBuffer *b) { return b->lo + b->capacity - b->hi; }
int text_byte(TextBuffer *b, unsigned long at) {
    if (at >= text_length(b)) return -1;
    return b->data[at < b->lo ? at : at + b->hi - b->lo] & 255;
}
static void release_group(struct Group *g) {
    struct Edit *e = g->first, *next;
    while (e) { next = e->next; free(e->old); free(e->newtext); free(e); e = next; }
    free(g);
}
static void clear_groups(struct Group *g) { while (g) { struct Group *next = g->next; release_group(g); g = next; } }
TextBuffer *text_new(void) {
    TextBuffer *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->linecap = 16; b->lines = calloc(b->linecap, sizeof(*b->lines));
    if (!b->lines) { free(b); return NULL; }
    b->nlines = 1; b->valid_states = b->cached_states = 1;
    return b;
}
void text_free(TextBuffer *b) {
    if (b) { allocated -= b->capacity; free(b->data); free(b->lines); free(b->wraps); clear_groups(b->undo); clear_groups(b->redo); free(b); }
}
int text_copy(TextBuffer *b, unsigned long at, unsigned long n, unsigned char *out) {
    unsigned long first;
    if (at > text_length(b) || n > text_length(b) - at || (n && !out)) return error(b, 2);
    first = at < b->lo ? b->lo - at : 0;
    if (first > n) first = n;
    if (first) memcpy(out, b->data + at, first);
    if (n > first) memcpy(out + first, b->data + at + first + b->hi - b->lo, n - first);
    return 0;
}
static int reserve(TextBuffer *b, unsigned long size, unsigned long lines) {
    unsigned long cap, tail = b->capacity - b->hi;
    unsigned char *data; struct Line *index;
    if (size > TEXT_MAX) return error(b, 3);
    if (lines > TEXT_LINES) return error(b, 5);
    if (lines > b->linecap) {
        cap = b->linecap;
        while (cap < lines) cap *= 2;
        index = realloc(b->lines, cap * sizeof(*index));
        if (!index) return error(b, 1);
        b->lines = index; b->linecap = cap;
    }
    if (size <= b->capacity) return 0;
    cap = b->capacity ? b->capacity : 4096;
    while (cap < size) cap *= 2;
    if (cap > TEXT_MAX) cap = TEXT_MAX;
    if (allocated - b->capacity + cap > 2097152UL) return error(b, 3);
    data = malloc(cap);
    if (!data) return error(b, 1);
    if (b->lo) memcpy(data, b->data, b->lo);
    if (tail) memcpy(data + cap - tail, b->data + b->hi, tail);
    allocated += cap - b->capacity;
    free(b->data); b->data = data; b->capacity = cap; b->hi = cap - tail;
    return 0;
}
unsigned long text_lines(TextBuffer *b) { return b->nlines; }
unsigned long text_start(TextBuffer *b, unsigned long row) { return row < b->nlines ? b->lines[row].at : text_length(b); }
unsigned long text_endline(TextBuffer *b, unsigned long row) {
    unsigned long end = row + 1 < b->nlines ? b->lines[row + 1].at - 1 : text_length(b);
    if (row + 1 < b->nlines && end > b->lines[row].at && text_byte(b, end - 1) == '\r') --end;
    return end;
}
unsigned long text_row(TextBuffer *b, unsigned long at) {
    unsigned long lo = 0, hi = b->nlines;
    if (at > text_length(b)) at = text_length(b);
    while (lo + 1 < hi) { unsigned long mid = lo + (hi - lo) / 2; if (b->lines[mid].at <= at) lo = mid; else hi = mid; }
    return lo;
}
static void gap(TextBuffer *b, unsigned long at) {
    if (at < b->lo) { unsigned long n = b->lo - at; memmove(b->data + b->hi - n, b->data + at, n); b->hi -= n; b->lo = at; }
    else if (at > b->lo) { unsigned long n = at - b->lo; memmove(b->data + b->lo, b->data + b->hi, n); b->hi += n; b->lo = at; }
}
static void replace_raw(TextBuffer *b, unsigned long at, unsigned long remove, const unsigned char *s, unsigned long n) {
    unsigned long row = text_row(b, at), end = text_row(b, at + remove), after = end + 1, added = 0, i, next;
    long delta = (long)n - (long)remove;
    if (row < b->wrap_rows) {
        unsigned long lo = 0, hi = b->wrap_count;
        while (lo < hi) { unsigned long mid = lo + (hi - lo) / 2; if (b->wraps[mid].row < row) lo = mid + 1; else hi = mid; }
        b->wrap_count = lo; b->wrap_rows = row;
    }
    for (i = 0; i < n; ++i) if (s[i] == '\n') ++added;
    /* Preserve the old suffix's incoming states across a line splice. Damage
     * accumulates across all edits/undo groups until the renderer consumes it. */
    if (b->cached_states > end + 1) b->cached_states = b->cached_states - (end - row) + added;
    else if (b->cached_states > row + 1) b->cached_states = row + 1;
    if (b->dirty_until > end + 1) b->dirty_until = b->dirty_until - (end - row) + added;
    else if (b->dirty_until > row) b->dirty_until = row + added + 1;
    if (!b->dirty_until || row < b->dirty_first) b->dirty_first = row;
    if (b->dirty_until < row + added + 1) b->dirty_until = row + added + 1;
    if (end != row || added) b->dirty_layout = 1;
    if (added != end - row)
        memmove(b->lines + row + added + 1, b->lines + after, (b->nlines - after) * sizeof(*b->lines));
    next = b->nlines - (after - row - 1) + added;
    for (i = row + added + 1; i < next; ++i) b->lines[i].at = (unsigned long)((long)b->lines[i].at + delta);
    next = row + 1;
    for (i = 0; i < n; ++i) if (s[i] == '\n') b->lines[next++].at = at + i + 1;
    b->nlines = b->nlines - (end - row) + added;
    if (b->valid_states > row + 1) b->valid_states = row + 1;
    gap(b, at); b->hi += remove;
    if (n) memcpy(b->data + b->lo, s, n);
    b->lo += n;
    ++b->generation;
}
static int prepare(TextBuffer *b, unsigned long at, unsigned long remove, const unsigned char *s, unsigned long n) {
    unsigned long i, rows = b->nlines, size = text_length(b);
    b->error = 0;
    if (at > size || remove > size - at || (n && !s)) return error(b, 2);
    if (n > TEXT_MAX || size - remove > TEXT_MAX - n) return error(b, 3);
    for (i = 0; i < n; ++i) {
        if (!s[i] || s[i] > 255) return error(b, 4);
        if (s[i] == '\n') ++rows;
    }
    for (i = at; i < at + remove; ++i) if (text_byte(b, i) == '\n') --rows;
    return reserve(b, size - remove + n, rows);
}
int text_append(TextBuffer *b, const unsigned char *s, unsigned long n) {
    unsigned long at = text_length(b);
    if (prepare(b, at, 0, s, n) < 0) return -1;
    if (!b->newline_known) {
        unsigned long i;
        for (i = 0; i < n; ++i) if (s[i] == '\n') {
            b->crlf = i ? s[i - 1] == '\r' : at && text_byte(b, at - 1) == '\r';
            b->newline_known = 1; break;
        }
    }
    replace_raw(b, at, 0, s, n); return 0;
}
void text_begin(TextBuffer *b, unsigned long cursor) { b->open_group = 0; b->begin_cursor = cursor; }
void text_end(TextBuffer *b, unsigned long cursor) { if (b->open_group && b->undo) b->undo->end = cursor; b->open_group = 0; b->begin_cursor = cursor; }
static void prune(TextBuffer *b) {
    while (b->undo && b->undo->next && (b->groups > GROUP_MAX || b->history_bytes > HISTORY_MAX)) {
        struct Group *p = b->undo;
        while (p->next->next) p = p->next;
        b->history_bytes -= p->next->bytes; --b->groups; release_group(p->next); p->next = NULL;
    }
}
int text_replace(TextBuffer *b, unsigned long at, unsigned long remove, const unsigned char *s, unsigned long n) {
    struct Edit *e; struct Group *g = b->undo; int fresh = !b->open_group;
    if (prepare(b, at, remove, s, n) < 0) return -1;
    if (!remove && !n) return 0;
    if (remove + n > HISTORY_MAX) return error(b, 6);
    if (!fresh && g->bytes + remove + n > HISTORY_MAX) fresh = 1;
    e = calloc(1, sizeof(*e));
    if (!e) return error(b, 1);
    if (remove) e->old = malloc(remove);
    if (n) e->newtext = malloc(n);
    if ((remove && !e->old) || (n && !e->newtext)) { free(e->old); free(e->newtext); free(e); return error(b, 1); }
    if (fresh) {
        g = calloc(1, sizeof(*g));
        if (!g) { free(e->old); free(e->newtext); free(e); return error(b, 1); }
        g->before = b->revision; g->after = b->sequence + 1; g->cursor = b->begin_cursor;
    }
    if (remove) text_copy(b, at, remove, e->old);
    if (n) memcpy(e->newtext, s, n);
    e->at = at; e->removed = remove; e->added = n;
    clear_groups(b->redo); b->redo = NULL;
    if (fresh) { g->next = b->undo; b->undo = g; ++b->groups; ++b->sequence; b->open_group = 1; }
    e->prev = g->last;
    if (g->last) g->last->next = e; else g->first = e;
    g->last = e; g->end = at + n; g->bytes += remove + n; b->history_bytes += remove + n;
    replace_raw(b, at, remove, e->newtext, n); b->revision = g->after;
    prune(b); return 0;
}
void text_saved(TextBuffer *b) { if (b->open_group && b->undo) b->begin_cursor = b->undo->end; b->open_group = 0; b->saved = b->revision; }
unsigned long text_revision(TextBuffer *b) { return b->revision; }
unsigned long text_generation(TextBuffer *b) { return b->generation; }
int text_modified(TextBuffer *b) { return b->revision != b->saved; }
long text_undo(TextBuffer *b) {
    struct Group *g = b->undo; struct Edit *e;
    b->open_group = 0;
    if (!g) return -1;
    for (e = g->last; e; e = e->prev) replace_raw(b, e->at, e->added, e->old, e->removed);
    b->undo = g->next; g->next = b->redo; b->redo = g;
    b->history_bytes -= g->bytes; --b->groups; b->revision = g->before; b->begin_cursor = g->cursor;
    return (long)g->cursor;
}
long text_redo(TextBuffer *b) {
    struct Group *g = b->redo; struct Edit *e;
    b->open_group = 0;
    if (!g) return -1;
    for (e = g->first; e; e = e->next) replace_raw(b, e->at, e->removed, e->newtext, e->added);
    b->redo = g->next; g->next = b->undo; b->undo = g;
    b->history_bytes += g->bytes; ++b->groups; b->revision = g->after; b->begin_cursor = g->end;
    return (long)g->end;
}
#include "unicode.inc"
#include "wrap.inc"
unsigned long text_visual(TextBuffer *b, unsigned long at, unsigned tabstop) {
    unsigned long start = text_start(b, text_row(b, at)), column = 0;
    unsigned width;
    if (!tabstop) tabstop = 4;
    at = text_boundary(b, at);
    while (start < at) {
        unsigned long next = cluster(b, start, text_length(b), &width, NULL);
        column += text_byte(b, start) == '\t' ? tabstop - column % tabstop : width;
        start = next;
    }
    return column;
}
unsigned long text_column(TextBuffer *b, unsigned long row, unsigned long column, unsigned tabstop) {
    unsigned long at = text_start(b, row), end = text_endline(b, row), x = 0, next, following;
    unsigned width;
    if (!tabstop) tabstop = 4;
    while (at < end) { following = cluster(b, at, end, &width, NULL); next = x + (text_byte(b, at) == '\t' ? tabstop - x % tabstop : width); if (next > column) break; x = next; at = following; }
    return at;
}
static int wordclass(TextBuffer *b, unsigned long at, int big) {
    uint32_t c; unsigned long next;
    if (at >= text_length(b)) return 0;
    c = scalar_at(b, at, text_length(b), &next);
    if (unicode_space(c)) return 0;
    return big || c >= 128 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ? 1 : 2;
}
/* Reverse Unicode word motion scans a line forward once, instead of seeking
 * every preceding grapheme from the start (quadratic on long UTF-8 words). */
static unsigned long previous_word(TextBuffer *b, unsigned long at, int big) {
    unsigned long original = at, row, p, candidate = 0; int c, prior, found;
    if (!at) return 0;
    if (text_byte(b, at - 1) >= 128) goto unicode;
    --at;
    while (at && !wordclass(b, at, big)) {
        if (text_byte(b, at - 1) >= 128) goto unicode;
        --at;
    }
    c = wordclass(b, at, big);
    while (at) {
        if (text_byte(b, at - 1) >= 128) goto unicode;
        if (wordclass(b, at - 1, big) != c) break;
        --at;
    }
    return at;
unicode:
    at = original; row = text_row(b, at);
    for (;;) {
        p = text_start(b, row); prior = found = 0;
        while (p < at) {
            c = wordclass(b, p, big);
            if (c && c != prior) { candidate = p; found = 1; }
            prior = c; p = cluster(b, p, text_length(b), NULL, NULL);
        }
        if (found) return candidate;
        if (!row) return 0;
        at = text_start(b, row--);
    }
}
unsigned long text_word(TextBuffer *b, unsigned long at, int motion, unsigned count) {
    unsigned long len = text_length(b); int c, big = motion == 'W' || motion == 'B' || motion == 'E';
    at = text_boundary(b, at);
    while (count--) {
        if (motion == 'b' || motion == 'B') {
            at = previous_word(b, at, big);
        } else if (motion == 'e' || motion == 'E') {
            if (at < len) at = cluster(b, at, len, NULL, NULL);
            while (at < len && !wordclass(b, at, big)) at = cluster(b, at, len, NULL, NULL);
            c = wordclass(b, at, big);
            while (at < len) { unsigned long next = cluster(b, at, len, NULL, NULL); if (next >= len || wordclass(b, next, big) != c) break; at = next; }
        } else {
            c = wordclass(b, at, big);
            while (at < len && wordclass(b, at, big) == c) at = cluster(b, at, len, NULL, NULL);
            while (at < len && !wordclass(b, at, big)) at = cluster(b, at, len, NULL, NULL);
        }
    }
    return at;
}
long text_find(TextBuffer *b, unsigned long at, const unsigned char *s, unsigned long n, int backward) {
    unsigned long len = text_length(b), pos, j, step;
    if (!n || n > len) return -1;
    if (at >= len) at = len - 1;
    for (step = 1; step <= len; ++step) {
        pos = backward ? (at + len - step) % len : (at + step) % len;
        if (n > len - pos) continue;
        for (j = 0; j < n && text_byte(b, pos + j) == s[j]; ++j) {}
        if (j == n) return (long)text_boundary(b, pos);
    }
    return -1;
}
static int cluster_matches(TextBuffer *b, unsigned long at, unsigned long end, unsigned long ch) {
    while (at < end) { unsigned long next; if (scalar_at(b, at, end, &next) == ch) return 1; at = next; }
    return 0;
}
long text_find_char(TextBuffer *b, unsigned long at, unsigned long ch, int backward, int till, unsigned count) {
    unsigned long start, end, p, next, matches = 0, wanted;
    at = text_boundary(b, at);
    start = text_start(b, text_row(b, at)); end = text_endline(b, text_row(b, at));
    if (backward && count) {
        /* Count/select in two forward passes, without a reverse-boundary seek
         * for every candidate or an unbounded stack array of matches. */
        for (p = start; p < at && p < end; p = next) {
            next = cluster(b, p, end, NULL, NULL);
            if (cluster_matches(b, p, next, ch)) ++matches;
        }
        if (matches < count) return -1;
        wanted = matches - count;
        for (p = start; p < at && p < end; p = next) {
            next = cluster(b, p, end, NULL, NULL);
            if (cluster_matches(b, p, next, ch) && !wanted--) { at = p; break; }
        }
    } else while (count--) {
        int found = 0;
        while (at < end) {
            at = cluster(b, at, end, NULL, NULL);
            if (at >= end) break;
            next = cluster(b, at, end, NULL, NULL);
            if (cluster_matches(b, at, next, ch)) { found = 1; break; }
        }
        if (!found) return -1;
    }
    return (long)(till ? backward ? text_next(b, at) : text_previous(b, at) : at);
}
long text_newline(TextBuffer *b, unsigned long at, int indent) {
    unsigned long start = text_start(b, text_row(b, at)), n = 0, i, length; unsigned char *s; int result;
    if (indent) while (start + n < at && (text_byte(b, start + n) == ' ' || text_byte(b, start + n) == '\t')) ++n;
    length = n + 1 + b->crlf;
    s = malloc(length); if (!s) return error(b, 1);
    if (b->crlf) s[0] = '\r';
    s[b->crlf] = '\n';
    for (i = 0; i < n; ++i) s[i + 1 + b->crlf] = text_byte(b, start + i);
    result = text_replace(b, at, 0, s, length); free(s);
    return result == 0 ? (long)(at + length) : -1;
}
long text_open_line(TextBuffer *b, unsigned long at, int above, int indent) {
    unsigned long row = text_row(b, at), start = text_start(b, row), n = 0, i; unsigned char *s; int result;
    if (!above) return text_newline(b, text_endline(b, row), indent);
    if (indent) while (start + n < text_endline(b, row) && (text_byte(b, start + n) == ' ' || text_byte(b, start + n) == '\t')) ++n;
    s = malloc(n + 1 + b->crlf); if (!s) return error(b, 1);
    for (i = 0; i < n; ++i) s[i] = text_byte(b, start + i);
    if (b->crlf) s[n] = '\r'; s[n + b->crlf] = '\n';
    result = text_replace(b, start, 0, s, n + 1 + b->crlf); free(s);
    return result == 0 ? (long)(start + n) : -1;
}

int text_indent(TextBuffer *b, unsigned long first, unsigned long last, unsigned width, int remove, int tabs) {
    unsigned long at, end, pos, n = 0, skip, i, rows; unsigned char *s; int result;
    if (first >= b->nlines || last < first || width < 1 || width > 16) return error(b, 2);
    if (last >= b->nlines) last = b->nlines - 1;
    at = text_start(b, first); end = last + 1 < b->nlines ? text_start(b, last + 1) : text_length(b);
    rows = last - first + 1;
    if (!remove && rows > (TEXT_MAX - (end - at)) / width) return error(b, 3);
    s = malloc(end - at + (remove ? 0 : rows * width) + 1); if (!s) return error(b, 1);
    pos = at;
    while (pos < end || (pos == end && pos == text_start(b, last))) {
        if (remove) {
            skip = 0;
            if (text_byte(b, pos) == '\t') ++pos;
            else while (pos < end && skip < width && text_byte(b, pos) == ' ') { ++pos; ++skip; }
        } else if (tabs) s[n++] = '\t';
        else for (i = 0; i < width; ++i) s[n++] = ' ';
        while (pos < end) { int c = text_byte(b, pos++); s[n++] = c; if (c == '\n') break; }
        if (pos == end) break;
    }
    result = text_replace(b, at, end - at, s, n); free(s); return result;
}
/* Return the end of a string/comment beginning at p, or p for code. This
 * deliberately shares lexical conventions with the lightweight highlighter. */
static int identifier(int c);
static unsigned long structural_skip(TextBuffer *b, unsigned long p) {
    unsigned long q, len = text_length(b); int c = text_byte(b, p), next = text_byte(b, p + 1), close = 0, two = 0;
    if (b->language == 15) {
        int raw = c == 'r' && next == '"';
        if (raw || c == '"') {
            for (q = p + (raw ? 2 : 1); q < len; ++q) {
                c = text_byte(b, q);
                if (!raw && c == '\\') { if (q + 1 < len) ++q; continue; }
                if (c == '"') return q + 1;
            }
            return len;
        }
        if (c != ';' && !(c == '/' && (next == '/' || next == '*'))) return p;
    }
    if (b->language == 13) {
        if (c=='\'' || ((c=='R'||c=='r') && (next=='E'||next=='e') &&
            (text_byte(b,p+2)=='M'||text_byte(b,p+2)=='m') &&
            !identifier(text_byte(b,p+3)) && text_byte(b,p+3)!='$' &&
            (p==0 || !identifier(text_byte(b,p-1))))) {
            for (q=p+1;q<len && text_byte(b,q)!='\n';++q) {} return q;
        }
        if (c!='"') return p;
        for (q=p+1;q<len;++q) {
            c=text_byte(b,q);
            if (c=='\n') return q;
            if (c=='"') { if (text_byte(b,q+1)=='"') ++q; else return q+1; }
        }
        return len;
    }
    if ((b->language == 1 || b->language == 14 || b->language == 15 || b->language == 10 || b->language == 4) && c == '/' && next == '/') {
        for (q = p + 2; q < len && text_byte(b, q) != '\n'; ++q) {} return q;
    }
    if (((b->language == 2 || b->language == 3 || b->language == 6 || b->language == 9 || b->language == 12) && c == '#') ||
        ((b->language == 6 || b->language == 11 || b->language == 12 || b->language == 15) && c == ';')) {
        for (q = p + 1; q < len && text_byte(b, q) != '\n'; ++q) {} return q;
    }
    if ((b->language == 1 || b->language == 14 || b->language == 15 || b->language == 10) && c == '/' && next == '*') { close = '/'; two = 1; }
    else if (b->language == 4 && c == '(' && next == '*') { close = ')'; two = 1; }
    else if (b->language == 4 && c == '{') close = '}';
    if (close) {
        for (q = p + (two ? 2 : 1); q < len; ++q)
            if (text_byte(b, q) == close && (!two || text_byte(b, q - 1) == '*')) return q + 1;
        return len;
    }
    if (c == '\'' || c == '"' || c == '`') {
        int triple = b->language == 3 && next == c && text_byte(b, p + 2) == c;
        q = p + (triple ? 3 : 1);
        while (q < len) {
            if (text_byte(b, q) == '\\') { q += 2; continue; }
            if (text_byte(b, q) == c && (!triple || (text_byte(b, q + 1) == c && text_byte(b, q + 2) == c))) return q + (triple ? 3 : 1);
            if (!triple && c != '`' && text_byte(b, q) == '\n') return q;
            ++q;
        }
        return len;
    }
    return p;
}
/* Structural objects ignore escaped quoted strings. Bracket objects choose the
 * innermost enclosing pair, or the next pair on the same line. Comments
 * recognized for the active language are skipped as well. */
int text_object(TextBuffer *b, unsigned long at, int object, int around, unsigned long *bounds) {
    unsigned long len = text_length(b), lo, hi, p, open = 0, depth = 0, next = len, starts[256];
    int c, left = 0, right = 0, quote = 0, escaped = 0;
    if (at > len) return error(b, 2);
    if (object == 'w' || object == 'W') {
        int big = object == 'W', cls;
        at = text_boundary(b, at); cls = wordclass(b, at, big);
        if (!cls) return error(b, 7);
        lo = at; hi = at;
        if (at < len) { unsigned long before = text_next(b, at); lo = previous_word(b, before, big); }
        while (hi < len && wordclass(b, hi, big) == cls) hi = cluster(b, hi, len, NULL, NULL);
        if (around) { p = hi; while (hi < len && (text_byte(b, hi) == ' ' || text_byte(b, hi) == '\t')) ++hi; if (p == hi) while (lo && (text_byte(b, lo - 1) == ' ' || text_byte(b, lo - 1) == '\t')) --lo; }
        bounds[0] = lo; bounds[1] = hi; return 0;
    }
    if (object == '\'' || object == '"' || object == '`') {
        lo = text_start(b, text_row(b, at)); hi = text_endline(b, text_row(b, at));
        for (p = lo; p < hi; ++p) {
            c = text_byte(b, p);
            if (escaped) { escaped = 0; continue; }
            if (c == '\\') { escaped = 1; continue; }
            if (c == object) { if (!quote) { open = p; quote = 1; } else { if (open <= at && p >= at) { bounds[0] = open + !around; bounds[1] = p + around; return 0; } if (open > at) { bounds[0] = open + !around; bounds[1] = p + around; return 0; } quote = 0; } }
        }
        return error(b, 7);
    }
    if (object == '(' || object == ')') left = '(', right = ')';
    else if (object == '[' || object == ']') left = '[', right = ']';
    else if (object == '{' || object == '}') left = '{', right = '}';
    else if (object == '<' || object == '>') left = '<', right = '>';
    else return error(b, 7);
    for (p = 0; p < len; ++p) {
        c = text_byte(b, p);
        { unsigned long skipped = structural_skip(b, p); if (skipped > p) { p = skipped - 1; continue; } }
        if (c == left) { if (depth == 256) return error(b, 7); starts[depth++] = p; }
        else if (c == right && depth) {
            open = starts[--depth];
            if (open <= at && p >= at) { bounds[0] = open + !around; bounds[1] = p + around; return 0; }
            if (open > at && open < next && text_row(b, open) == text_row(b, at)) { bounds[0] = open + !around; bounds[1] = p + around; next = open; }
        }
    }
    return next < len ? 0 : error(b, 7);
}
long text_match(TextBuffer *b, unsigned long at) {
    unsigned long p, len = text_length(b), stack[256], depth = 0;
    int c = text_byte(b, at), left, right;
    if (c == '(' || c == ')') left = '(', right = ')';
    else if (c == '[' || c == ']') left = '[', right = ']';
    else if (c == '{' || c == '}') left = '{', right = '}';
    else if (c == '<' || c == '>') left = '<', right = '>';
    else return -1;
    for (p = 0; p < len; ++p) {
        unsigned long skipped = structural_skip(b, p);
        if (skipped > p) { p = skipped - 1; continue; }
        c = text_byte(b, p);
        if (c == left) { if (depth == 256) return -1; stack[depth++] = p; }
        else if (c == right && depth) { unsigned long open = stack[--depth]; if (open == at) return (long)p; if (p == at) return (long)open; }
    }
    return -1;
}
unsigned long text_hash(const unsigned char *bytes, unsigned long n, unsigned long seed) {
    unsigned long i, h = seed;
    for (i = 0; i < n; ++i) h = ((h ^ bytes[i]) * 16777619UL) & 0xffffffffUL;
    return h;
}
#include "syntax.inc"
