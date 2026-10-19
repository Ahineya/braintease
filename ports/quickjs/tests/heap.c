/* SPDX-License-Identifier: MIT
 * Production QuickJS pools over the production C16 allocation callbacks.
 * Does not create a JSRuntime or evaluate JavaScript.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "cutils.h"
#include "list.h"
#include "qjs-platform.h"
#include "heap-routines.inc"

static unsigned checks, failures;
static void check(int ok, const char *label)
{
    ++checks;
    if (!ok) { ++failures; printf("FAIL %s\n", label); }
}
static void setup(JSMallocContext *s, QJSAllocatorControl *control)
{
    js_malloc_init(s); s->mf = qjs_c16_malloc_functions;
    s->malloc_state.malloc_limit = SIZE_MAX; s->malloc_state.opaque = control;
}
static void empty(JSMallocContext *s)
{
    unsigned i;
    int clean = !s->malloc_state.malloc_count && !s->malloc_state.malloc_size;
    for (i = 0; i < JS_MALLOC_BLOCK_SIZE_COUNT; ++i)
        if (!list_empty(&s->arena_list[i]) || !list_empty(&s->free_arena_list[i])) clean = 0;
    check(clean, "all production arenas and large blocks released");
}
static void metadata(JSMallocContext *s, void *p)
{
    JSMallocBlockHeader *h = js_rc(p);
    JSValue v;
    check(!((uintptr_t)p & 3U) && !atom_is_free(p), "production payload preserves low pointer tag bits");
    h->ref_count = 70000L; h->gc_obj_type = 6; h->mark = 1;
    v = JS_DupValue(NULL, JS_MKPTR(JS_TAG_OBJECT, p));
    check(JS_VALUE_GET_PTR(v) == p && h->ref_count == 70001L &&
          h->gc_obj_type == 6 && h->mark == 1,
          "public reference ABI addresses production header without clobbering GC bits");
    (void)s;
}
static void classes(JSMallocContext *s)
{
    unsigned index;
    size_t length, usable, i;
    unsigned char *p, *q;
    check(sizeof(JSMallocBlockHeader) == 8 &&
          offsetof(JSMallocBlockHeader, user_data) == sizeof(JSMallocBlockHeader) &&
          offsetof(JSMallocBlockHeader, ref_count) + sizeof(JSRefCountHeader) == sizeof(JSMallocBlockHeader),
          "C16 pool header layout keeps reference count immediately before payload");
    check(!(offsetof(JSMallocArena, blocks) & 3U), "arena block storage preserves four cell alignment");
    p = __js_malloc(s, 0); q = __js_malloc(s, 0);
    check(p && p == q && !__js_malloc_usable_size(s, (char *)p), "production zero size singleton");
    __js_free(s, p); __js_free(s, q); __js_free(s, NULL);
    for (index = 0; index < JS_MALLOC_BLOCK_SIZE_COUNT; ++index) {
        length = js_malloc_block_sizes[index] - sizeof(JSMallocBlockHeader);
        p = __js_malloc(s, length);
        check(p && js_rc(p)->block_size_idx == index && js_rc(p)->u.block_idx != FREE_NIL &&
              __js_malloc_usable_size(s, (char *)p) >= length, "every production size class allocates from its pool");
        if (!p) continue;
        metadata(s, p); usable = __js_malloc_usable_size(s, (char *)p);
        for (i = 0; i < usable; ++i) p[i] = (unsigned char)(i ^ 0xbeefU);
        check(p[usable - 1] == (unsigned char)((usable - 1) ^ 0xbeefU), "pool usable size is writable");
        __js_free(s, p);
    }
    empty(s);
}
static void resize(JSMallocContext *s, QJSAllocatorControl *control)
{
    unsigned char *p, *q;
    size_t i, calls, live;
    p = __js_malloc(s, 7); if (!p) { check(0, "resize initial allocation"); return; }
    metadata(s, p); for (i = 0; i < 7; ++i) p[i] = 0xfaceU + i;
    q = __js_realloc(s, p, 8);
    check(q == p, "same size class realloc retains pointer");
    q = __js_realloc(s, p, 40);
    check(q && q[6] == 0xfad4U && js_rc(q)->ref_count == 70001L &&
          js_rc(q)->gc_obj_type == 6 && js_rc(q)->mark == 1,
          "pool to pool realloc preserves payload reference and GC metadata");
    if (q) p = q;
    q = __js_realloc(s, p, 70000UL);
    check(q && q[6] == 0xfad4U && js_rc(q)->u.block_idx == FREE_NIL &&
          js_rc(q)->ref_count == 70001L && __js_malloc_usable_size(s, (char *)q) == 70000UL,
          "pool to large realloc preserves metadata and full 32 bit size");
    if (q) p = q; else { __js_free(s, p); return; }
    metadata(s, p);
    p[65536UL] = 0xabcd; p[69999UL] = 0x1234;
    calls = control->calls; live = s->malloc_state.malloc_size;
    q = __js_realloc(s, p, SIZE_MAX - 3);
    check(!q && control->calls == calls && s->malloc_state.malloc_size == live &&
          p[65536UL] == 0xabcd && p[69999UL] == 0x1234,
          "overflow fails before provider and leaves old allocation valid");
    control->fail_at = control->calls + 1;
    q = __js_realloc(s, p, 90000UL);
    check(!q && p[69999UL] == 0x1234 && js_rc(p)->ref_count == 70001L,
          "failed large growth preserves old payload and metadata");
    control->fail_at = 0;
    q = __js_realloc(s, p, 90000UL);
    check(q && !((uintptr_t)q & 3U) && q[65536UL] == 0xabcd && q[69999UL] == 0x1234 &&
          js_rc(q)->ref_count == 70001L && js_rc(q)->mark,
          "large growth realignment preserves cross bank data and headers");
    if (q) p = q;
    q = __js_realloc(s, p, 6);
    check(q && q[0] == 0xface && q[5] == 0xfad3 && js_rc(q)->ref_count == 70001L,
          "large shrink preserves prefix and reference metadata");
    if (q) p = q;
    check(!__js_realloc(s, p, 0), "realloc zero releases production allocation");
    calls = control->calls;
    check(!__js_malloc(s, SIZE_MAX) && !__js_malloc(s, SIZE_MAX - 3) && control->calls == calls,
          "allocation rounding overflow cannot become a small allocation");
    empty(s);
}
static void churn(JSMallocContext *s, QJSAllocatorControl *control, size_t request, unsigned class_index)
{
    void **blocks = malloc(4000UL * sizeof(void *));
    size_t i, count = 0, arenas = 0;
    struct list_head *el;
    int good = 1;
    if (!blocks) { check(0, "churn pointer table allocation"); return; }
    for (i = 0; i < 4000UL; ++i) {
        unsigned char *p = __js_malloc(s, request), *temporary;
        if (!p) break;
        blocks[count++] = p; p[0] = (unsigned char)i; p[request - 1] = (unsigned char)(i ^ 0xffffU);
        if ((uintptr_t)p & 3U) good = 0;
        temporary = __js_malloc(s, request);
        if (!temporary) good = 0;
        else { memset(temporary, 0x5a, request); __js_free(s, temporary); }
    }
    list_for_each(el, &s->arena_list[class_index]) { ++arenas; }
    check(count == 4000UL && arenas > 2 && good, "multiple full production arenas preserve alignment");
    for (i = 0; i < count; ++i) {
        unsigned char *p = blocks[i];
        if (p[0] != (unsigned char)i || p[request - 1] != (unsigned char)(i ^ 0xffffU)) good = 0;
    }
    check(good, "production arena payloads remain distinct across banks");
    for (i = 0; i < count; i += 2) { __js_free(s, blocks[i]); blocks[i] = NULL; }
    control->fail_at = control->calls + 1;
    /* Existing pool space remains reusable even when a new provider call fails. */
    for (i = 0; i < count; i += 2) {
        blocks[i] = __js_malloc(s, request);
        if (!blocks[i]) good = 0;
    }
    check(good, "freed pool slots are reusable under provider allocation failure");
    control->fail_at = 0;
    for (i = 0; i < count; ++i) __js_free(s, blocks[i]);
    free(blocks); empty(s);
}
static void tags(JSMallocContext *s)
{
    uint32_t values[] = { 0, 1, 32768UL, 65535UL, 70000UL, 0x3fffffffUL };
    unsigned i;
    for (i = 0; i < countof(values); ++i) {
        JSAtomStruct *p = atom_set_free(values[i]);
        check(atom_is_free(p) && atom_get_free(p) == values[i], "atom free indices preserve all 30 bits");
    }
    (void)s;
}
int main(int argc, char **argv)
{
    JSMallocContext s;
    QJSAllocatorControl control = { 0, 0, 0 };
    setup(&s, &control); classes(&s); resize(&s, &control);
    churn(&s, &control, 8, 0); churn(&s, &control, 18, 2); tags(&s);
    if (argc > 1 && !strcmp(argv[1], "--negative-control")) check(0, "expected negative control");
    printf("QUICKJS_HEAP checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
