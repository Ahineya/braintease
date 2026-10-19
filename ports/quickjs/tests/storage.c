/* SPDX-License-Identifier: MIT
 * Real core string/serialization routines, with guarded test allocation and
 * minimal context/exception plumbing. No JSRuntime creation or JS evaluation.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdarg.h>
#include "cutils.h"
#include "quickjs.h"
#ifdef QJS_C16_H
typedef long test_int;
#else
typedef int test_int;
typedef uint8_t qjs_octet_t;
#endif
typedef struct JSString JSString;
#define JS_VALUE_GET_STRING(v) ((JSString *)JS_VALUE_GET_PTR(v))
#define JS_STRING_LEN_MAX ((1UL << 30) - 1)
#define JS_ATOM_empty_string 1
#define bc_read_trace(...)

struct JSRuntime { size_t live, calls, fail_at, slack; };
struct JSContext { JSRuntime *rt; test_int error; };
typedef struct {
    size_t size;
    uint32_t guard;
    JSRefCountHeader ref; /* immediately before returned storage */
} TestHeader;
typedef struct { JSContext *ctx; DynBuf dbuf; } BCWriterState;
typedef struct {
    JSContext *ctx;
    const qjs_octet_t *buf_start, *ptr, *buf_end;
    test_int error_state;
} BCReaderState;
static unsigned checks, failures;
static void check(int ok, const char *label)
{
    ++checks;
    if (!ok) { ++failures; printf("FAIL %s\n", label); }
}
static TestHeader *test_header(const void *p) { return (TestHeader *)p - 1; }
static JSRefCountHeader *js_rc(void *p) { return &test_header(p)->ref; }
static void guards(void *p)
{
    TestHeader *h = test_header(p);
    qjs_octet_t *tail = (qjs_octet_t *)p + h->size;
    unsigned i;
    if (h->guard != 0x51735452UL) { puts("FAIL allocation prefix guard"); abort(); }
    for (i = 0; i < 16; ++i)
        if (tail[i] != 0x5a) { puts("FAIL allocation suffix guard"); abort(); }
}
void *js_malloc_rt(JSRuntime *rt, size_t size)
{
    TestHeader *h;
    if (++rt->calls == rt->fail_at || size > 300000UL) return NULL;
    size += rt->slack;
    h = malloc(sizeof(*h) + size + 16);
    if (!h) return NULL;
    h->size = size; h->guard = 0x51735452UL; h->ref.ref_count = 1;
    memset(h + 1, 0x5a, size + 16);
    ++rt->live;
    return h + 1;
}
void js_free_rt(JSRuntime *rt, void *ptr)
{
    if (!ptr) return;
    guards(ptr); --rt->live; free(test_header(ptr));
}
void js_free(JSContext *ctx, void *ptr) { js_free_rt(ctx->rt, ptr); }
size_t js_malloc_usable_size(JSContext *ctx, const void *ptr)
{
    (void)ctx; return test_header(ptr)->size;
}
void *js_realloc_rt(JSRuntime *rt, void *ptr, size_t size)
{
    void *fresh;
    size_t old;
    if (!ptr) return js_malloc_rt(rt, size);
    guards(ptr); old = test_header(ptr)->size;
    fresh = js_malloc_rt(rt, size);
    if (!fresh) return NULL;
    memcpy(fresh, ptr, old < size ? old : size);
    js_free_rt(rt, ptr);
    return fresh;
}
void *js_realloc2(JSContext *ctx, void *p, size_t n, size_t *slack)
{
    void *fresh = js_realloc_rt(ctx->rt, p, n);
    if (!fresh) { JS_ThrowOutOfMemory(ctx); return NULL; }
    *slack = test_header(fresh)->size - n;
    return fresh;
}
static void JS_FreeAtomStruct(JSRuntime *rt, JSString *p)
{
    (void)rt; (void)p; puts("FAIL unexpected atom plumbing"); abort();
}
/* Definitions after the generated routines, using their actual JSString. */
static JSString *js_alloc_string(JSContext *, test_int, test_int);
#include "storage-routines.inc"

JSValue JS_ThrowOutOfMemory(JSContext *ctx) { ctx->error = 1; return JS_EXCEPTION; }
JSValue JS_ThrowInternalError(JSContext *ctx, const char *format, ...)
{
    (void)format; ctx->error = 2; return JS_EXCEPTION;
}
JSValue JS_ThrowSyntaxError(JSContext *ctx, const char *format, ...)
{
    (void)format; ctx->error = 3; return JS_EXCEPTION;
}
JSValue JS_ToString(JSContext *ctx, JSValueConst value)
{
    (void)value; return JS_ThrowInternalError(ctx, "fixture accepts strings only");
}
JSValue JS_AtomToString(JSContext *ctx, JSAtom atom)
{
    JSString *p;
    assert(atom == JS_ATOM_empty_string);
    p = js_alloc_string(ctx, 0, 0);
    if (!p) return JS_EXCEPTION;
    p->u.str8[0] = 0;
    return JS_MKPTR(JS_TAG_STRING, p);
}
void __JS_FreeValue(JSContext *ctx, JSValue value)
{
    assert(JS_IsString(value)); js_free_rt(ctx->rt, JS_VALUE_GET_PTR(value));
}

static void record(JSContext *ctx, JSValue value)
{
    JSString *s = JS_VALUE_GET_STRING(value);
    const char *utf8;
    size_t n;
    test_int i;
    printf("S:%lu:%lu:", (unsigned long)s->len, (unsigned long)hash_string(s, 1));
    for (i = 0; i < s->len && i < 12; ++i) printf("%04lx,", (unsigned long)string_get(s, i));
    utf8 = JS_ToCStringLen2(ctx, &n, value, 0);
    assert(utf8);
    printf(":%lu:", (unsigned long)n);
    for (i = 0; i < n && i < 24; ++i) printf("%02x", (unsigned)(qjs_octet_t)utf8[i]);
    putchar('\n'); JS_FreeCString(ctx, utf8);
}

static void strings(JSContext *ctx)
{
    /* Numeric arrays avoid depending on host/compiler source encoding. */
    const char utf8[] = { 'A', 0xc3, 0xa9, 0xd0, 0x96, 0xe4, 0xb8, 0xad,
                          0xf0, 0x9f, 0x98, 0x80, 0, 'Z' };
    const uint16_t units[] = { 'A', 0xe9, 0x416, 0x4e2d, 0xd83d, 0xde00, 0, 'Z' };
    const qjs_octet_t cesu[] = { 0xed, 0xa0, 0xbd, 0xed, 0xb8, 0x80 };
    const uint16_t lone[] = { 0xd800, 'x', 0xdc00 };
    const uint16_t hi[] = { 0xffff }, lo[] = { 0 };
    const char malformed[] = { 'x', 0xc0, 0x80, 'y', 0xe2, 0x82 };
    const char *bytes;
    size_t n, before;
    JSValue a, b, c, d;
    JSString *p;
    test_int i, index;
    int ok;
    a = JS_NewStringLen(ctx, utf8, sizeof(utf8));
    check(!JS_IsException(a), "UTF8 construction");
    if (JS_IsException(a)) return;
    p = JS_VALUE_GET_STRING(a);
    check(p->len == 8 && p->is_wide_char, "UTF16 code unit length including NUL");
    ok = 1;
    for (i = 0; i < 8; ++i) if (string_get(p, i) != units[i]) ok = 0;
    check(ok, "UTF8 to UTF16 preserves BMP and supplementary characters");
    record(ctx, a);
    bytes = JS_ToCStringLen2(ctx, &n, a, 0);
    check(bytes && n == sizeof(utf8) && !memcmp(bytes, utf8, n) && bytes[n] == 0,
          "length aware UTF8 export round trip");
    JS_FreeCString(ctx, bytes);
    b = js_sub_string(ctx, p, 4, 6);
    bytes = JS_ToCStringLen2(ctx, &n, b, 1);
    check(bytes && n == 6 && !memcmp(bytes, cesu, n), "CESU8 surrogate export");
    JS_FreeCString(ctx, bytes); JS_FreeValue(ctx, b);
    index = 4;
    check(string_getc(p, &index) == 0x1f600L && index == 6 &&
          string_advance_index(p, 4, 1) == 6 && string_advance_index(p, 4, 0) == 5,
          "code point iteration differs from code unit iteration");
    b = js_new_string16_len(ctx, units, 8);
    check(js_string_eq(ctx, p, JS_VALUE_GET_STRING(b)), "native UTF16 copy");
    record(ctx, b);
    c = js_sub_string(ctx, p, 0, p->len);
    check(JS_VALUE_GET_PTR(c) == p && js_rc(p)->ref_count == 2, "substring retains full string");
    JS_FreeValue(ctx, c); /* a, b distinct; a had one additional reference */
    /* The b allocation does not retain a. */
    check(js_rc(p)->ref_count == 1, "substring reference release");
    c = js_sub_string(ctx, p, 0, 2);
    check(JS_VALUE_GET_STRING(c)->len == 2 && !JS_VALUE_GET_STRING(c)->is_wide_char &&
          string_get(JS_VALUE_GET_STRING(c), 1) == 0xe9, "substring narrows Latin1 safely");
    record(ctx, c); JS_FreeValue(ctx, c);
    c = js_new_string16_len(ctx, hi, 1); d = js_new_string16_len(ctx, lo, 1);
    check(js_string_compare(ctx, JS_VALUE_GET_STRING(d), JS_VALUE_GET_STRING(c)) < 0 &&
          js_string_compare(ctx, JS_VALUE_GET_STRING(c), JS_VALUE_GET_STRING(d)) > 0,
          "UTF16 comparison widens before unsigned subtraction");
    JS_FreeValue(ctx, c); JS_FreeValue(ctx, d);
    c = js_new_string_char(ctx, 0xff);
    check(js_string_compare(ctx, JS_VALUE_GET_STRING(c), p) > 0 &&
          js_string_compare(ctx, p, JS_VALUE_GET_STRING(c)) < 0, "mixed Latin1 and UTF16 comparison");
    JS_FreeValue(ctx, c);
    c = js_new_string16_len(ctx, lone, 3);
    check(js_string_find_invalid_codepoint(JS_VALUE_GET_STRING(c)) == 0,
          "unpaired surrogates remain representable");
    record(ctx, c); JS_FreeValue(ctx, c);
    c = JS_NewStringLen(ctx, malformed, sizeof(malformed));
    record(ctx, c); JS_FreeValue(ctx, c);
#ifdef QJS_C16_H
    before = ctx->rt->live;
    c = JS_NewStringLen(ctx, "", JS_STRING_LEN_MAX + 1);
    check(JS_IsException(c) && ctx->error && ctx->rt->live == before,
          "oversize input rejected before reading or narrowing length");
    ctx->error = 0;
#endif
    c = JS_ConcatString1(ctx, p, JS_VALUE_GET_STRING(b));
    check(!JS_IsException(c) && JS_VALUE_GET_STRING(c)->len == 16 &&
          string_get(JS_VALUE_GET_STRING(c), 13) == 0xde00,
          "wide concatenation copies native units");
    JS_FreeValue(ctx, c);
    JS_FreeValue(ctx, a); JS_FreeValue(ctx, b);
}

static void buffers(JSContext *ctx)
{
    StringBuffer b;
    JSValue value, tail;
    JSString *p;
    test_int i;
    int ok;
    uint16_t extra[] = { 0xffff, 0, 0xd83d, 0xde00 };
    check(!string_buffer_init(ctx, &b, 1) && !string_buffer_puts8(&b, "hello"),
          "Latin1 buffer growth");
    check(!string_buffer_write16(&b, extra, 4) && b.is_wide_char && b.len == 9,
          "widen buffer preserving prefix");
    check(!string_buffer_putc(&b, 0x1f642UL) && b.len == 11,
          "buffer appends supplementary code point as pair");
    check(!string_buffer_fill(&b, '!', 20), "wide buffer fill and growth");
    value = string_buffer_end(&b); p = JS_VALUE_GET_STRING(value);
    check(p->len == 31 && p->u.str16[0] == 'h' && p->u.str16[5] == 0xffff &&
          p->u.str16[9] == 0xd83d && p->u.str16[10] == 0xde42 && p->u.str16[30] == '!',
          "buffer finalization and shrink");
    record(ctx, value);
    JS_FreeValue(ctx, value);
    ctx->rt->slack = 17;
    check(!string_buffer_init(ctx, &b, 1) && !string_buffer_putc16(&b, 0x416),
          "widen with allocator slack");
    /* Assert in native units; C16 and host have different character sizes. */
    check((size_t)b.size == 1 + 17 / sizeof(uint16_t), "wide slack converted to code units");
    for (i = 1; i < 50; ++i) string_buffer_putc16(&b, (uint32_t)(0x416 + i));
    value = string_buffer_end(&b); p = JS_VALUE_GET_STRING(value);
    ok = p->len == 50;
    for (i = 0; i < 50; ++i) if (p->u.str16[i] != 0x416 + i) ok = 0;
    check(ok, "slack capacity never exceeds allocated storage");
    tail = js_new_string16_len(ctx, extra, 4);
    check(JS_ConcatStringInPlace(ctx, p, tail) && p->len == 54 && p->u.str16[53] == 0xde00,
          "in place wide append within usable allocation");
    JS_FreeValue(ctx, tail); JS_FreeValue(ctx, value); ctx->rt->slack = 0;
    /* Large native string exercises pointer arithmetic across C16 banks. */
    check(!string_buffer_init2(ctx, &b, 70000L, 1), "bank spanning UTF16 allocation");
    for (i = 0; i < 70000L; ++i) string_buffer_putc16(&b, 0x400UL + (i % 64));
    value = string_buffer_end(&b); p = JS_VALUE_GET_STRING(value);
    check(p->len == 70000L && string_get(p, 65536L) == 0x400 &&
          string_get(p, 69999L) == 0x42f, "bank spanning UTF16 indexing");
    tail = js_sub_string(ctx, p, 63990L, 64010L);
    check(JS_VALUE_GET_STRING(tail)->len == 20 &&
          string_get(JS_VALUE_GET_STRING(tail), 10) == string_get(p, 64000L),
          "substring copies across bank boundary");
    record(ctx, value); JS_FreeValue(ctx, tail); JS_FreeValue(ctx, value);
    check(!string_buffer_init(ctx, &b, 0), "empty buffer initialization");
    value = string_buffer_end(&b);
    check(!JS_IsException(value) && !JS_VALUE_GET_STRING(value)->len, "empty buffer result");
    JS_FreeValue(ctx, value);
}

static void serialization(JSContext *ctx)
{
    BCWriterState w;
    BCReaderState r;
    JSValue value;
    JSString *p, *copy;
    uint16_t units[] = { 'A', 0x416, 0xd83d, 0xde00, 0, 0xffff };
    uint16_t u16;
    uint32_t u32;
    uint64_t u64;
    int32_t s32;
    size_t cut, before;
    unsigned i;
    w.ctx = ctx; dbuf_init(&w.dbuf);
    bc_put_u8(&w, 0xab); bc_put_u16(&w, 0x89ab); bc_put_u32(&w, 0x89abcdefUL);
    bc_put_u64(&w, 0xfedcba9876543210ULL);
    bc_put_leb128(&w, 0xffffffffUL); bc_put_sleb128(&w, -1234567L);
    check(w.dbuf.size == 24 && w.dbuf.buf[7] == 0x10 && w.dbuf.buf[14] == 0xfe,
          "serialized numeric widths are octets rather than native sizeof");
    printf("B:"); for (i = 0; i < w.dbuf.size; ++i) printf("%02x", (unsigned)w.dbuf.buf[i]); putchar('\n');
    memset(&r, 0, sizeof(r)); r.ctx = ctx; r.ptr = w.dbuf.buf; r.buf_end = r.ptr + w.dbuf.size;
    { qjs_octet_t u8; check(!bc_get_u8(&r, &u8) && u8 == 0xab, "read serialized octet"); }
    check(!bc_get_u16(&r, &u16) && u16 == 0x89ab, "read serialized uint16");
    check(!bc_get_u32(&r, &u32) && u32 == 0x89abcdefUL, "read serialized uint32");
    check(!bc_get_u64(&r, &u64) && u64 == 0xfedcba9876543210ULL, "read serialized uint64");
    check(!bc_get_leb128(&r, &u32) && u32 == 0xffffffffUL &&
          !bc_get_sleb128(&r, &s32) && s32 == -1234567L && r.ptr == r.buf_end,
          "read serialized variable length integers");
    check(bc_get_u16(&r, &u16) < 0 && !u16 && r.error_state < 0,
          "truncated operand fails with initialized output");
    dbuf_free(&w.dbuf); ctx->error = 0;
    value = js_new_string16_len(ctx, units, 6); p = JS_VALUE_GET_STRING(value);
    dbuf_init(&w.dbuf); JS_WriteString(&w, p);
    check(w.dbuf.size == 13 && w.dbuf.buf[0] == 13 && w.dbuf.buf[3] == 0x16 &&
          w.dbuf.buf[4] == 4 && w.dbuf.buf[11] == 0xff, "wide string serialization is little endian UTF16");
    printf("B:"); for (i = 0; i < w.dbuf.size; ++i) printf("%02x", (unsigned)w.dbuf.buf[i]); putchar('\n');
    memset(&r, 0, sizeof(r)); r.ctx = ctx; r.ptr = w.dbuf.buf; r.buf_end = r.ptr + w.dbuf.size;
    copy = JS_ReadString(&r);
    check(copy && js_string_eq(ctx, p, copy) && r.ptr == r.buf_end,
          "deserialize octets into native UTF16 storage");
    if (copy) js_free_string(ctx->rt, copy);
    before = ctx->rt->live;
    for (cut = 0; cut < w.dbuf.size; ++cut) {
        memset(&r, 0, sizeof(r)); r.ctx = ctx; r.ptr = w.dbuf.buf; r.buf_end = r.ptr + cut;
        copy = JS_ReadString(&r);
        check(!copy && r.error_state < 0 && ctx->rt->live == before,
              "every truncated wide string rejects without leaking");
        if (copy) js_free_string(ctx->rt, copy);
        ctx->error = 0;
    }
    dbuf_free(&w.dbuf); JS_FreeValue(ctx, value);
}

static void failures_test(JSContext *ctx)
{
    StringBuffer b;
    JSValue value;
    const char *bytes;
    size_t length;
    size_t before = ctx->rt->live;
    ctx->rt->fail_at = ctx->rt->calls + 1;
    check(string_buffer_init(ctx, &b, 4) < 0 && JS_IsException(string_buffer_end(&b)),
          "buffer initial allocation failure");
    string_buffer_free(&b); ctx->rt->fail_at = 0;
    string_buffer_init(ctx, &b, 4); string_buffer_puts8(&b, "abc");
    ctx->rt->fail_at = ctx->rt->calls + 1;
    check(string_buffer_putc16(&b, 0x416) < 0 && JS_IsException(string_buffer_end(&b)) &&
          ctx->rt->live == before, "widen failure releases old storage");
    string_buffer_free(&b); ctx->rt->fail_at = 0;
    ctx->error = 0;
    value = JS_NewStringLen(ctx, "recovered", 9);
    check(!JS_IsException(value) && JS_VALUE_GET_STRING(value)->len == 9,
          "string subsystem recovers after allocation failure");
    JS_FreeValue(ctx, value);
    ctx->error = 0;
    string_buffer_init(ctx, &b, 1); string_buffer_putc8(&b, 'x');
    ctx->rt->fail_at = ctx->rt->calls + 1;
    check(string_buffer_puts8(&b, "growth") < 0 && JS_IsException(string_buffer_end(&b)) &&
          ctx->rt->live == before, "buffer growth failure releases storage");
    string_buffer_free(&b); ctx->rt->fail_at = 0; ctx->error = 0;
    string_buffer_init2(ctx, &b, 100, 1); string_buffer_putc16(&b, 0x416);
    ctx->rt->fail_at = ctx->rt->calls + 1;
    value = string_buffer_end(&b);
    check(!JS_IsException(value) && JS_VALUE_GET_STRING(value)->len == 1 &&
          string_get(JS_VALUE_GET_STRING(value), 0) == 0x416,
          "failed optional shrink retains valid string");
    JS_FreeValue(ctx, value); ctx->rt->fail_at = 0;
#ifdef QJS_C16_H
    value = js_new_string_char(ctx, 0x416);
    ctx->rt->fail_at = ctx->rt->calls + 1; length = 99;
    bytes = JS_ToCStringLen2(ctx, &length, value, 0);
    check(!bytes && !length && js_rc(JS_VALUE_GET_PTR(value))->ref_count == 1,
          "failed UTF8 export releases temporary string reference");
    JS_FreeCString(ctx, bytes); JS_FreeValue(ctx, value);
    ctx->rt->fail_at = 0; ctx->error = 0;
#endif
}

static void bytecode(void)
{
    const int32_t values[] = { INT32_MIN, -65536L, -32769L, -32768L, -129, -128,
        -127, -2, -1, 0, 7, 8, 127, 128, 255, 256, 32767, 32768L, 65535L,
        70000L, INT32_MAX };
    DynBuf b;
    CodeContext c;
    int32_t decoded, v;
    size_t i, j, expected;
    qjs_octet_t op;
    for (i = 0; i < countof(values); ++i) {
        v = values[i]; dbuf_init(&b); push_short_int(&b, v); op = b.buf[0];
        expected = v >= -1 && v <= 7 ? 1 : v >= -128 && v <= 127 ? 2 :
                   v >= -32768L && v <= 32767 ? 3 : 5;
        if (op == OP_push_i8) decoded = get_i8(b.buf + 1);
        else if (op == OP_push_i16) decoded = get_i16(b.buf + 1);
        else if (op == OP_push_i32) decoded = get_i32(b.buf + 1);
        else decoded = (int32_t)op - OP_push_0;
        check(b.size == expected && decoded == v, "smallest bytecode immediate preserves value");
        printf("C:%ld:", (long)v);
        for (j = 0; j < b.size; ++j) printf("%02x", (unsigned)b.buf[j]);
        putchar('\n'); dbuf_free(&b);
    }
    dbuf_init(&b);
    dbuf_putc(&b, OP_line_num); dbuf_put_u32(&b, 70000UL);
    dbuf_putc(&b, OP_get_loc); dbuf_put_u16(&b, 60000U); dbuf_putc(&b, OP_return);
    memset(&c, 0, sizeof(c)); c.bc_buf = b.buf; c.bc_len = b.size;
    check(code_match(&c, 0, OP_get_loc, -1, OP_return, -1) && c.idx == 60000L &&
          c.line_num == 70000L && c.pos == b.size,
          "pattern enum literals wildcards and 32 bit line numbers");
    check(code_match(&c, 0, OP_get_loc, 60000L, OP_return, -1), "pattern explicit wide operand");
    check(!code_match(&c, 0, OP_get_loc, 12, OP_return, -1), "pattern operand mismatch");
    for (i = 0; i < 5; ++i) {
        c.bc_len = i;
        check(!code_match(&c, 0, OP_get_loc, -1, -1), "pattern rejects truncated line operand");
    }
    dbuf_free(&b);
    dbuf_init(&b); dbuf_putc(&b, OP_get_field); dbuf_put_u32(&b, 0x89abcdefUL);
    c.bc_buf = b.buf; c.bc_len = b.size;
    check(code_match(&c, 0, M4(OP_nop, OP_drop, OP_get_field, OP_return), -1) &&
          c.op == OP_get_field && c.atom == 0x89abcdefUL,
          "four opcode alternatives retain high packed octets");
    check(!code_match(&c, 0, M3(OP_nop, OP_drop, OP_return), -1), "opcode alternatives mismatch");
    dbuf_free(&b);
    for (i = 0; i < 3; ++i) {
        const test_int indexes[] = { 3, 255, 60000L };
        dbuf_init(&b); put_short_code(&b, OP_get_loc, indexes[i]);
        check(b.size == (i == 0 ? 1 : i == 1 ? 2 : 3) &&
              (i == 0 ? b.buf[0] == OP_get_loc3 : i == 1 ? b.buf[1] == 255 :
               get_u16(b.buf + 1) == 60000L), "local index opcode and operand width");
        dbuf_free(&b);
    }
}

int main(int argc, char **argv)
{
    JSRuntime rt = { 0, 0, 0, 0 };
    JSContext ctx;
    ctx.rt = &rt; ctx.error = 0;
    check(offsetof(TestHeader, ref) + sizeof(JSRefCountHeader) == sizeof(TestHeader),
          "fixture reference header matches public ABI");
    strings(&ctx); buffers(&ctx); serialization(&ctx); failures_test(&ctx); bytecode();
    check(!rt.live, "all string allocations released");
    if (argc > 1 && !strcmp(argv[1], "--negative-control")) check(0, "expected negative control");
    printf("QUICKJS_STORAGE checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
