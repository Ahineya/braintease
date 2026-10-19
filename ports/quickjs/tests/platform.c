/* SPDX-License-Identifier: MIT
 * C16 embedding ABI and platform hooks. This fixture does not evaluate JS.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qjs-platform.h"

static unsigned checks, failures;
static volatile unsigned stack_cookie;
static void check(int ok, const char *label)
{
    ++checks;
    if (!ok) { ++failures; printf("FAIL %s\n", label); }
}

static JSValue callback(JSContext *ctx, JSValueConst self, long argc,
                         JSValueConst *argv)
{
    (void)ctx; (void)self;
    return JS_NewInt32(NULL, argc + JS_VALUE_GET_INT(argv[0]));
}

static JSValue iterator(JSContext *ctx, JSValueConst self, long argc,
                         JSValueConst *argv, long *done, long magic)
{
    (void)ctx; (void)self; (void)argv;
    *done = argc;
    return JS_NewInt32(NULL, magic);
}

static long interrupt(JSRuntime *rt, void *opaque)
{
    (void)rt; (void)opaque;
    return 65536L;
}

static long define_property(JSContext *ctx, JSValueConst obj, JSAtom atom,
                             JSValueConst value, JSValueConst get,
                             JSValueConst set, long flags)
{
    (void)ctx; (void)obj; (void)atom; (void)value; (void)get; (void)set;
    return flags;
}

static unsigned descend(uintptr_t base, unsigned depth)
{
    volatile unsigned frame[128];
    unsigned result;
    frame[depth & 127] = depth;
    if (qjs_c16_check_stack(base, 2048UL, sizeof(frame))) result = depth;
    else if (depth == 100) result = 0;
    else result = descend(base, depth + 1);
    /* Keep this a real recursive stack frame, including across optimization. */
    stack_cookie += frame[depth & 127];
    return result;
}

static void abi(void)
{
    JSCFunction *function = callback;
    JSCFunctionType kind;
    JSInterruptHandler *poll = interrupt;
    JSClassExoticMethods methods;
    JSPropertyDescriptor descriptor;
    JSValue arg = JS_NewInt32(NULL, 123456L), value;
    struct { long done; uint32_t canary; } output = { 0, 0xabcdef89UL };
    check(sizeof(JS_BOOL) == sizeof(int32_t), "32-bit JS_BOOL ABI");
    check(sizeof(JSRefCountHeader) == sizeof(int32_t), "32-bit reference header");
    descriptor.flags = JS_PROP_NO_EXOTIC | JS_PROP_THROW_STRICT;
    check(descriptor.flags == 98304L, "property flags beyond bit 15");
    check(JS_PROP_THROW_STRICT == 32768L && JS_PROP_NO_EXOTIC == 65536L,
          "wide flag constant shifts");
    check(JS_DEFAULT_STACK_SIZE == 16384UL, "bounded default guest stack budget");
    value = function(NULL, JS_UNDEFINED, 70000L, &arg);
    check(JS_VALUE_GET_INT(value) == 193456L, "native function argument count ABI");
    kind.iterator_next = iterator;
    value = kind.iterator_next(NULL, JS_UNDEFINED, 70000L, &arg, &output.done, -80000L);
    check(output.done == 70000L && output.canary == 0xabcdef89UL &&
          JS_VALUE_GET_INT(value) == -80000L, "iterator output pointer and magic ABI");
    check(poll(NULL, NULL) == 65536L, "interrupt callback return ABI");
    memset(&methods, 0, sizeof(methods));
    methods.define_own_property = define_property;
    check(methods.define_own_property(NULL, JS_NULL, 0, JS_NULL, JS_NULL, JS_NULL,
                                    descriptor.flags) == 98304L,
          "exotic property callback flags and result ABI");
}

static void allocations(void)
{
    const JSMallocFunctions *mf = &qjs_c16_malloc_functions;
    QJSAllocatorControl control = { 0, 0, 0 };
    JSMallocState s = { 0, 0, 0, &control }, moved;
    size_t overhead = qjs_c16_allocation_overhead(), before, calls, i;
    unsigned char *p, *q;
    check(mf->js_malloc_usable_size(NULL) == 0, "NULL usable size");
    mf->js_free(&s, NULL);
    check(!s.malloc_count && !s.malloc_size, "NULL free");
    check(!mf->js_malloc(&s, 0) && !mf->js_realloc(&s, NULL, 0), "zero size allocations");
    check(!mf->js_malloc(&s, 1), "zero memory limit");
    s.malloc_limit = overhead + 10;
    p = mf->js_malloc(&s, 10);
    check(p && s.malloc_count == 1 && s.malloc_size == s.malloc_limit,
          "exact limit includes allocator header");
    if (!p) return;
    check(mf->js_malloc_usable_size(p) == 10, "usable size excludes allocator header");
    for (i = 0; i < 10; ++i) p[i] = (unsigned char)(50000UL + i);
    check(!mf->js_malloc(&s, 1), "cumulative allocation limit");
    before = s.malloc_size;
    q = mf->js_realloc(&s, p, 11);
    check(!q && s.malloc_count == 1 && s.malloc_size == before && p[9] == 50009U,
          "failed growth preserves allocation and accounting");
    if (q) p = q;
    s.malloc_limit = SIZE_MAX;
    control.fail_at = control.calls + 1;
    check(!mf->js_malloc(&s, 50) && s.malloc_count == 1 && s.malloc_size == before,
          "injected allocation failure");
    control.fail_at = control.calls + 1;
    q = mf->js_realloc(&s, p, 70000UL);
    check(!q && mf->js_malloc_usable_size(p) == 10 && p[9] == 50009U,
          "injected realloc failure keeps old allocation");
    if (q) p = q;
    control.fail_at = 0;
    q = mf->js_realloc(&s, p, 70000UL);
    check(q && mf->js_malloc_usable_size(q) == 70000UL &&
          s.malloc_count == 1 && s.malloc_size == overhead + 70000UL,
          "realloc grows across banks");
    if (!q) { mf->js_free(&s, p); return; }
    p = q;
    check(p[0] == 50000U && p[9] == 50009U, "growth preserves full C16 cells");
    p[69999UL] = 0xbeef;
    check(p[69999UL] == 0xbeef &&
          ((uintptr_t)p >> 16) != ((uintptr_t)(p + 69999UL) >> 16),
          "bank-spanning allocation is addressable");
    check(control.peak_cells == s.malloc_size, "peak allocation accounting");
    calls = control.calls;
    check(!mf->js_malloc(&s, SIZE_MAX) && !mf->js_realloc(&s, p, SIZE_MAX) &&
          control.calls == calls && p[69999UL] == 0xbeef,
          "header addition overflow rejected before allocator");
    s.malloc_limit = 1;
    q = mf->js_realloc(&s, p, 8);
    check(q && s.malloc_size == overhead + 8 && s.malloc_count == 1,
          "shrinking allowed after lowering memory limit");
    if (!q) { mf->js_free(&s, p); return; }
    p = q;
    check(p[7] == 50007U && mf->js_malloc_usable_size(p) == 8,
          "shrink preserves prefix and updates usable size");
    check(!mf->js_realloc(&s, p, 0) && !s.malloc_count && !s.malloc_size,
          "realloc to zero releases block");
    s.malloc_limit = overhead + 16;
    p = mf->js_realloc(&s, NULL, 16);
    check(p && s.malloc_count == 1, "realloc NULL allocation");
    /* JS_NewRuntime2 copies its initial JSMallocState into the new runtime. */
    moved = s;
    mf->js_free(&moved, p);
    check(!moved.malloc_count && !moved.malloc_size, "state can move into runtime");
    s = moved;
    control.calls = control.peak_cells = 0;
    s.malloc_limit = SIZE_MAX;
    for (i = 0; i < 80; ++i) {
        p = mf->js_malloc(&s, i + 1);
        if (!p) break;
        p[i] = 0xface;
        mf->js_free(&s, p);
    }
    check(i == 80 && !s.malloc_count && !s.malloc_size,
          "repeated allocation destruction has no live blocks");
    check(qjs_c16_physical_octets(70000UL) == 140000ULL &&
          qjs_c16_physical_octets(SIZE_MAX) == 8589934590ULL,
          "physical octet reporting widens before multiplication");
    s.opaque = NULL;
    p = mf->js_malloc(&s, 10);
    check(p && s.malloc_count == 1, "optional allocator instrumentation");
    mf->js_free(&s, p);
    check(!s.malloc_count && !s.malloc_size, "uninstrumented cleanup");
    for (i = 0; i < 12; ++i) {
        void *prefix = malloc(i + 1);
        p = mf->js_malloc(&s, i + 11);
        check(p && !((uintptr_t)p & 3U), "four cell alignment after odd sized allocations");
        if (!p) { free(prefix); break; }
        p[0] = 0xbeef; p[i + 10] = 0xabcd;
        q = mf->js_realloc(&s, p, 70000UL + i);
        check(q && !((uintptr_t)q & 3U) && q[0] == 0xbeef && q[i + 10] == 0xabcd,
              "reallocation realigns without losing the copied prefix");
        if (q) p = q;
        mf->js_free(&s, p); free(prefix);
    }
    check(!s.malloc_count && !s.malloc_size, "aligned allocation cleanup");
}

static void stacks(void)
{
    volatile unsigned marker = 0;
    uintptr_t base = qjs_c16_linear_address((const void *)&marker);
    unsigned depth;
    check(qjs_c16_linear_address((void *)0x0001f9ffUL) == 127999UL &&
          qjs_c16_linear_address((void *)0x00020000UL) == 128000UL,
          "adjacent banks have no artificial 1536-cell stack gap");
    check(!qjs_c16_stack_at(127999UL, 100, 98, 128000UL) &&
          qjs_c16_stack_at(127999UL, 100, 100, 128000UL),
          "bank boundary reserve accounting");
    check(!qjs_c16_stack_at(1000, 100, 50, 1050) &&
          qjs_c16_stack_at(1000, 100, 51, 1050), "upward stack growth and exact reserve");
    check(!qjs_c16_stack_at(1000, 100, 50, 950) &&
          qjs_c16_stack_at(1000, 100, 51, 950), "downward stack growth and exact reserve");
    check(qjs_c16_stack_at(1000, 100, 0, 1100) &&
          qjs_c16_stack_at(1000, 100, 0, 900), "stack limit reached without underflow");
    check(qjs_c16_stack_at(1000, 100, SIZE_MAX, 1001), "stack reservation overflow rejected");
    check(!qjs_c16_stack_at(0, 0, SIZE_MAX, SIZE_MAX), "explicit stack guard disable");
    check(!qjs_c16_check_stack(base, 2048UL, 0), "real guest stack starts within budget");
    depth = descend(base, 0);
    check(depth > 0 && depth < 100 && stack_cookie != 0,
          "real recursive guest stack trips guard and unwinds");
    check(!qjs_c16_check_stack(base, 2048UL, 0), "stack guard recovers after unwind");
}

int main(int argc, char **argv)
{
    abi(); allocations(); stacks();
    if (argc > 1 && !strcmp(argv[1], "--negative-control"))
        check(0, "expected negative control");
    printf("QUICKJS_PLATFORM checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
