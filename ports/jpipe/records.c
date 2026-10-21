/* SPDX-License-Identifier: MIT */
#include "jpipe.h"
#define UNREAD (-2)
static int peek(JReader *r) {
    if (r->look == UNREAD) r->look = fgetc(stdin);
    return r->look;
}
static int take(JReader *r) { int c = peek(r); r->look = UNREAD; return c; }
static int space(int c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }
static int nonspace(JReader *r) { int c; do { c = take(r); } while (space(c)); return c; }
static int append(JReader *r, int c) {
    size_t n; char *fresh;
    if (r->length == r->pipe->record_limit) return j_error(r->pipe, "record exceeds --max-record");
    if (r->length + 1 >= r->capacity) {
        n = r->capacity ? r->capacity : 256;
        if (n <= r->length + 1) {
            if (n > ((size_t)-1) / 2) return j_error(r->pipe, "record allocation overflow");
            n *= 2;
        }
        if (n > r->pipe->record_limit + 1) n = r->pipe->record_limit + 1;
        fresh = js_realloc(r->pipe->ctx, r->data, n);
        if (!fresh) return j_exception(r->pipe);
        r->data = fresh; r->capacity = n;
    }
    r->data[r->length++] = c; r->data[r->length] = 0; return 0;
}
static int emit(JReader *r, int text) {
    JSValue value; int result;
    r->pipe->active = r->pipe->count;
    j_budget(r->pipe);
    value = text ? JS_NewStringLen(r->pipe->ctx, r->data ? r->data : "", r->length) :
        JS_ParseJSON(r->pipe->ctx, r->data ? r->data : "", r->length, "<stdin>");
    if (JS_IsException(value)) return j_exception(r->pipe);
    result = j_push(r->pipe, 0, value);
    r->pipe->active = r->pipe->count;
    return result;
}
static int lines(JReader *r) {
    int c;
    for (;;) {
        r->length = 0; r->pipe->input_record++;
        while ((c = take(r)) != EOF && c != '\n') if (append(r, c)) return -1;
        if (c == EOF && !r->length) break;
        if (c == '\n' && r->length && r->data[r->length-1] == '\r') r->data[--r->length] = 0;
        if (emit(r, r->pipe->input == J_TEXT)) return -1;
        if (c == EOF) break;
    }
    return 0;
}
/* Frame one JSON value, never the whole root array. QuickJS validates each
 * frame; this scanner handles nesting/quoted escapes, not JSON semantics. */
static int value(JReader *r, int first) {
    int c, quoted = first == '"', escaped = 0;
    size_t depth = first == '[' || first == '{' ? 1 : 0;
    r->length = 0;
    if (first == EOF) return j_error(r->pipe, "unexpected EOF in JSON document");
    if (append(r, first)) return -1;
    if (!depth && !quoted) {
        while ((c = peek(r)) != EOF && !space(c) && c != ',' && c != ']' && c != '}')
            if (append(r, take(r))) return -1;
        return emit(r, 0);
    }
    for (;;) {
        c = take(r);
        if (c == EOF) return j_error(r->pipe, "unexpected EOF in JSON value");
        if (append(r, c)) return -1;
        if (quoted) {
            if (escaped) escaped = 0;
            else if (c == '\\') escaped = 1;
            else if (c == '"') { quoted = 0; if (!depth) break; }
        } else if (c == '"') quoted = 1;
        else if (c == '[' || c == '{') depth++;
        else if (c == ']' || c == '}') { if (!--depth) break; }
    }
    return emit(r, 0);
}
static int document(JReader *r) {
    int c = nonspace(r);
    r->pipe->input_record = 1;
    if (c != '[') { if (value(r, c)) return -1; }
    else {
        c = nonspace(r);
        if (c != ']') for (;;) {
            if (c == ']') return j_error(r->pipe, "trailing comma in JSON array");
            if (value(r, c)) return -1;
            c = nonspace(r);
            if (c == ']') break;
            if (c != ',') return j_error(r->pipe, "expected comma or closing JSON array bracket");
            r->pipe->input_record++; c = nonspace(r);
        }
    }
    if (nonspace(r) != EOF) return j_error(r->pipe, "trailing data after JSON document");
    return 0;
}
int j_read(JPipe *p) {
    JReader r; int result;
    memset(&r, 0, sizeof(r)); r.pipe = p; r.look = UNREAD;
    result = p->input == J_DOCUMENT ? document(&r) : lines(&r);
    if (!result && ferror(stdin)) result = j_error(p, "stdin read failed");
    js_free(p->ctx, r.data); return result;
}
