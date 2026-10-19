/* SPDX-License-Identifier: MIT
 * Experimental whole-engine runtime/object integration, not an embedding SDK.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qjs-platform.h"

typedef struct {
    QJSAllocatorControl allocation; /* platform instrumentation prefix */
    size_t live;
} ProbeControl;
static unsigned checks, failures;
static int negative_control, trace;
static void check(int ok, const char *label)
{
    ++checks;
    if (!ok) { ++failures; printf("FAIL %s\n", label); }
}
static void *allocate(JSMallocState *s, size_t size)
{
    ProbeControl *c = s->opaque;
    void *p = qjs_c16_malloc_functions.js_malloc(s, size);
    if (p) ++c->live;
    return p;
}
static void release(JSMallocState *s, void *p)
{
    ProbeControl *c = s->opaque;
    if (p) --c->live;
    qjs_c16_malloc_functions.js_free(s, p);
}
static void *resize(JSMallocState *s, void *p, size_t size)
{
    ProbeControl *c = s->opaque;
    void *q = qjs_c16_malloc_functions.js_realloc(s, p, size);
    if (!p && q) ++c->live;
    else if (p && !size) --c->live;
    return q;
}
static size_t usable(const void *p) { return qjs_c16_malloc_functions.js_malloc_usable_size(p); }
static const JSMallocFunctions functions = { allocate, release, resize, usable };

static JSValue native_add(JSContext *ctx, JSValueConst self, long argc, JSValueConst *argv)
{
    (void)self;
    if (argc == 1) return JS_NewBool(ctx, JS_IsUndefined(argv[1]));
    return JS_NewInt32(ctx, JS_VALUE_GET_INT(argv[0]) + JS_VALUE_GET_INT(argv[1]));
}

static void exception(JSContext *ctx)
{
    JSValue error = JS_GetException(ctx), message;
    const char *text;
    printf("RUNTIME: exception tag %ld\n", (long)JS_VALUE_GET_TAG(error));
    message = JS_IsObject(error) ? JS_GetPropertyStr(ctx, error, "message") : JS_DupValue(ctx, error);
    if (JS_IsString(message)) {
        text = JS_ToCString(ctx, message);
        printf("RUNTIME: %s\n", text ? text : "<string export failed>");
        JS_FreeCString(ctx, text);
    }
    JS_FreeValue(ctx, message); JS_FreeValue(ctx, error);
}

static long interrupt(JSRuntime *rt, void *opaque)
{
    unsigned *count = opaque;
    (void)rt;
    return ++*count >= 3;
}

static void evaluation(JSContext *ctx, int full)
{
    static const struct { const char *source; int32_t expected; } cases[] = {
        { "1 + 2", 3 },
        { "70000 + 42", 70042L },
        { "(function(a) { return function(b) { return a+b; }; })(70000)(42)", 70042L },
        { "let sum=0; for(let i=0;i<100;i++) sum+=i; sum", 4950 },
        { "'abc'.length", 3 },
        /* JavaScript escapes and literal UTF-8 are tested separately. Avoid C
         * hex escapes, whose compiler reencoding has its own regression. */
        { "'\\u0416\\ud83d\\ude00'.length", 3 },
        { "({answer:70042}).answer", 70042L },
        { "'use strict'; (function(){return this === void 0 ? 1 : 0;})()", 1 },
        { "'use\\x20strict'; (function(){return this === void 0 ? 1 : 0;})()", 0 },
        { "'this directive is deliberately too long'; 42", 42 },
        { "('use strict').length", 10 },
        { "'abc' + 'def' === 'abcdef' ? 1 : 0", 1 },
        { "'\\u0000x'.length", 2 },
        { "('Ж😀' === '\\u0416\\ud83d\\ude00') ? 1 : 0", 1 },
        { "((() => { let t='x'; return () => t; })())().length", 1 },
        { "(function(){let s=0;for(let i=0;i<300;i++)s+=i;return s;})()", 44850L },
        { "nativeAdd(70000,42)", 70042L },
        { "nativeAdd(123) ? 1 : 0", 1 },
        { "1.5 * 2", 3 },
    };
    static const char *invalid[] = { "let =", "'unterminated", "'use strict'; delete x;" };
    unsigned i;
    unsigned interrupts = 0;
    JSValue value, error;
    int32_t result;
    char *source;
    const size_t length = 70000UL;
    static const char tail[] = "'.length";
    puts("RUNTIME: enabling evaluation");
    if (!full && JS_AddIntrinsicEval(ctx) < 0) {
        check(0, "evaluation intrinsic creation"); exception(ctx); return;
    }
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        printf("RUNTIME: evaluating case %u\n", i);
        value = JS_Eval(ctx, cases[i].source, strlen(cases[i].source),
                        "<guest-probe>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(value)) {
            check(0, "guest evaluation succeeds"); exception(ctx); return;
        }
        check(JS_IsNumber(value) && !JS_ToInt32(ctx, &result, value) &&
              result == cases[i].expected + (negative_control && i == 0), "guest evaluation result");
        JS_FreeValue(ctx, value);
    }
    /* Parse a first-statement directive candidate across real memory banks.
     * Its source length cannot be narrowed to libc's 16-bit precision slot. */
    source = malloc(length + sizeof(tail) + 1);
    check(source != NULL, "cross bank evaluation source allocation");
    if (!source) return;
    source[0] = '\''; memset(source + 1, 'x', length);
    memcpy(source + length + 1, tail, sizeof(tail));
    value = JS_Eval(ctx, source, length + sizeof(tail), "<cross-bank>", JS_EVAL_TYPE_GLOBAL);
    free(source);
    check(!JS_IsException(value) && JS_IsNumber(value) &&
          !JS_ToInt32(ctx, &result, value) && result == 70000L,
          "guest parses and evaluates a string literal crossing a memory bank");
    if (JS_IsException(value)) exception(ctx);
    JS_FreeValue(ctx, value);
    value = JS_Eval(ctx, "throw 42", strlen("throw 42"), "<expected-error>", JS_EVAL_TYPE_GLOBAL);
    check(JS_IsException(value), "guest throw propagates an exception");
    JS_FreeValue(ctx, value);
    error = JS_GetException(ctx);
    check(JS_IsNumber(error) && JS_VALUE_GET_INT(error) == 42, "thrown guest value is preserved");
    JS_FreeValue(ctx, error);
    value = JS_Eval(ctx, "6*7", 3, "<recovery>", JS_EVAL_TYPE_GLOBAL);
    check(!JS_IsException(value) && JS_VALUE_GET_INT(value) == 42, "evaluation recovers after exception");
    if (JS_IsException(value)) exception(ctx);
    JS_FreeValue(ctx, value);
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        value = JS_Eval(ctx, invalid[i], strlen(invalid[i]), "<invalid>", JS_EVAL_TYPE_GLOBAL);
        check(JS_IsException(value), "malformed source is rejected");
        JS_FreeValue(ctx, value);
        error = JS_GetException(ctx);
        check(JS_IsObject(error), "syntax failure produces an error object");
        JS_FreeValue(ctx, error);
    }
    JS_SetInterruptHandler(JS_GetRuntime(ctx), interrupt, &interrupts);
    value = JS_Eval(ctx, "while(true){}", strlen("while(true){}"), "<interrupt>", JS_EVAL_TYPE_GLOBAL);
    JS_SetInterruptHandler(JS_GetRuntime(ctx), NULL, NULL);
    check(JS_IsException(value) && interrupts >= 3, "guest loop is interrupted through the embedding callback");
    JS_FreeValue(ctx, value);
    error = JS_GetException(ctx); JS_FreeValue(ctx, error);
    value = JS_Eval(ctx, "6*7", 3, "<recovery>", JS_EVAL_TYPE_GLOBAL);
    check(!JS_IsException(value) && JS_VALUE_GET_INT(value) == 42, "evaluation recovers after syntax errors and interruption");
    if (JS_IsException(value)) exception(ctx);
    JS_FreeValue(ctx, value);
    puts("RUNTIME: evaluation completed");
}

static JSValue native_assert(JSContext *ctx, JSValueConst self, long argc, JSValueConst *argv)
{
    const char *label;
    int ok = argc > 0 && JS_ToBool(ctx, argv[0]);
    (void)self;
    label = argc > 1 ? JS_ToCString(ctx, argv[1]) : NULL;
    if (trace) printf("BUILTIN: %s\n", label ? label : "<missing>");
    check(ok, label ? label : "guest assertion");
    JS_FreeCString(ctx, label);
    if (!ok) return JS_ThrowInternalError(ctx, "guest assertion failed");
    return JS_UNDEFINED;
}
static JSValue native_gc(JSContext *ctx, JSValueConst self, long argc, JSValueConst *argv)
{
    (void)self; (void)argc; (void)argv;
    JS_RunGC(JS_GetRuntime(ctx)); return JS_UNDEFINED;
}
static JSValue native_bank_offset(JSContext *ctx, JSValueConst self, long argc, JSValueConst *argv)
{
    size_t size, edge;
    qjs_octet_t *data;
    (void)self; (void)argc;
    data = JS_GetArrayBuffer(ctx, &size, argv[0]);
    if (!data) return JS_EXCEPTION;
    if (size < 70016UL) return JS_ThrowRangeError(ctx, "bank probe needs a larger buffer");
    edge = 64000UL - ((uintptr_t)data & 65535UL);
    if (edge < 3) edge += 64000UL;
    return JS_NewUint32(ctx, edge - 3);
}
static void builtins(JSContext *ctx)
{
    static const char source[] =
#include "builtin-fixture.inc"
    ;
    JSValue global = JS_GetGlobalObject(ctx), result;
    JSContext *job_ctx;
    long jobs = 0, rc;
    puts("RUNTIME: checking standard built-ins");
    JS_SetPropertyStr(ctx, global, "nativeAssert", JS_NewCFunction(ctx, native_assert, "nativeAssert", 2));
    JS_SetPropertyStr(ctx, global, "nativeGC", JS_NewCFunction(ctx, native_gc, "nativeGC", 0));
    JS_SetPropertyStr(ctx, global, "nativeBankOffset", JS_NewCFunction(ctx, native_bank_offset, "nativeBankOffset", 1));
    result = JS_Eval(ctx, source, sizeof(source) - 1, "builtins.js", JS_EVAL_TYPE_GLOBAL);
    check(!JS_IsException(result), "standard built-in script completes");
    if (JS_IsException(result)) { exception(ctx); JS_FreeValue(ctx, global); return; }
    JS_FreeValue(ctx, result);
    while ((rc = JS_ExecutePendingJob(JS_GetRuntime(ctx), &job_ctx)) > 0 && ++jobs < 64) {}
    check(rc == 0 && jobs == 2, "Promise jobs drain within the embedding budget");
    if (rc < 0) exception(job_ctx);
    result = JS_GetPropertyStr(ctx, global, "asyncDone");
    check(JS_ToBool(ctx, result), "Promise chain completes");
    JS_FreeValue(ctx, result); JS_FreeValue(ctx, global);
    puts("RUNTIME: built-ins completed");
}

static void context(JSRuntime *rt, int eval)
{
    JSContext *ctx;
    JSValue object, value, function, args[2], global;
    JSAtom atoms[1200], duplicate, numeric;
    unsigned i;
    int good = 1;
    char name[32];
    const char *text;
    const char unicode[] = { 0xd0, 0x96, 0xf0, 0x9f, 0x98, 0x80, 0 };
    puts(eval == 2 ? "RUNTIME: creating full context" : "RUNTIME: creating raw context");
    ctx = eval == 2 ? JS_NewContext(rt) : JS_NewContextRaw(rt);
    check(ctx != NULL, "production context and intrinsic creation");
    if (!ctx) return;
    puts("RUNTIME: context ready");
    object = JS_NewObject(ctx);
    check(!JS_IsException(object), "ordinary object creation");
    if (JS_SetPropertyStr(ctx, object, unicode, JS_NewInt32(ctx, 70000L)) < 0) {
        check(0, "UTF8 property name creates a production atom and shape");
        exception(ctx); JS_FreeValue(ctx, object); JS_FreeContext(ctx); return;
    }
    check(1, "UTF8 property name creates a production atom and shape");
    puts("RUNTIME: Unicode property inserted");
    value = JS_GetPropertyStr(ctx, object, unicode);
    check(JS_IsNumber(value) && JS_VALUE_GET_INT(value) == 70000L, "Unicode property lookup");
    JS_FreeValue(ctx, value);
    puts("RUNTIME: Unicode property read");
    numeric = JS_NewAtom(ctx, "70000");
    check(numeric == 0x80011170UL, "numeric atom preserves bit 31 and 32 bit index");
    text = JS_AtomToCString(ctx, numeric);
    check(text && !strcmp(text, "70000"), "numeric atom string export");
    JS_FreeCString(ctx, text); JS_FreeAtom(ctx, numeric);
    check(JS_SetPropertyUint32(ctx, object, 70000UL, JS_NewInt32(ctx, 42)) >= 0,
          "tagged numeric property insertion");
    value = JS_GetPropertyUint32(ctx, object, 70000UL);
    check(JS_VALUE_GET_INT(value) == 42, "tagged numeric property lookup");
    JS_FreeValue(ctx, value);
    for (i = 0; i < 1200; ++i) {
        snprintf(name, sizeof(name), "probe-%u", i);
        atoms[i] = JS_NewAtom(ctx, name);
        duplicate = JS_NewAtom(ctx, name);
        if (!atoms[i] || duplicate != atoms[i]) good = 0;
        JS_FreeAtom(ctx, duplicate);
    }
    check(good, "atom table and hash grow while retaining stable duplicate identities");
    for (i = 0; i < 1200; ++i) JS_FreeAtom(ctx, atoms[i]);
    duplicate = JS_NewAtom(ctx, "reused-slot");
    check(duplicate != 0, "free atom chain is reusable after table growth");
    JS_FreeAtom(ctx, duplicate);
    function = JS_NewCFunction(ctx, native_add, "nativeAdd", 2);
    args[0] = JS_NewInt32(ctx, 70000L); args[1] = JS_NewInt32(ctx, 42);
    value = JS_Call(ctx, function, JS_UNDEFINED, 2, args);
    check(!JS_IsException(value) && JS_VALUE_GET_INT(value) == 70042L, "native callback through production object call path");
    JS_FreeValue(ctx, value);
    value = JS_Call(ctx, function, JS_UNDEFINED, 1, args);
    check(JS_IsBool(value) && JS_VALUE_GET_BOOL(value), "production callback padding and checked frame cleanup");
    JS_FreeValue(ctx, value);
    if (eval) {
        global = JS_GetGlobalObject(ctx);
        check(JS_SetPropertyStr(ctx, global, "nativeAdd", JS_DupValue(ctx, function)) >= 0,
              "native callback is installed in the guest global object");
        JS_FreeValue(ctx, global);
    }
    JS_FreeValue(ctx, function);
    if (eval) evaluation(ctx, eval == 2);
    if (eval == 2) builtins(ctx);
    check(JS_SetPropertyStr(ctx, object, "self", JS_DupValue(ctx, object)) >= 0,
          "reference cycle creation");
    JS_FreeValue(ctx, object); JS_RunGC(rt);
    JS_FreeContext(ctx); JS_RunGC(rt);
    puts("RUNTIME: context and cycle collected");
}

int main(int argc, char **argv)
{
    ProbeControl control;
    JSRuntime *rt;
    size_t calls, i;
    int eval = 0;
    for (i = 1; i < (size_t)argc; ++i) {
        if (!strcmp(argv[i], "--eval")) eval = 1;
        else if (!strcmp(argv[i], "--full")) eval = 2;
        else if (!strcmp(argv[i], "--trace")) trace = 1;
        else if (!strcmp(argv[i], "--negative-control")) negative_control = 1;
    }
    memset(&control, 0, sizeof(control));
    puts("RUNTIME: creating full engine runtime");
    rt = JS_NewRuntime2(&functions, &control);
    check(rt != NULL, "full production runtime creation");
    if (!rt) goto done;
    check(control.live > 1 && control.allocation.peak_cells > 0, "runtime uses bounded instrumented allocator");
    JS_SetRuntimeOpaque(rt, &control);
    check(JS_GetRuntimeOpaque(rt) == &control && !JS_IsJobPending(rt), "fresh runtime state and opaque pointer");
    JS_RunGC(rt);
    calls = control.allocation.calls;
    if (eval == 2) JS_SetMaxStackSize(rt, QJS_C16_TERMINAL_STACK_CELLS);
    context(rt, eval);
    JS_FreeRuntime(rt);
    check(!control.live, "runtime teardown releases all production allocations");
    for (i = 1; i <= calls; ++i) {
        memset(&control, 0, sizeof(control)); control.allocation.fail_at = i;
        rt = JS_NewRuntime2(&functions, &control);
        if (rt) JS_FreeRuntime(rt);
        check(!control.live, "runtime construction failure unwinds all allocations");
    }
    for (i = 0; i < 5; ++i) {
        memset(&control, 0, sizeof(control));
        rt = JS_NewRuntime2(&functions, &control);
        check(rt != NULL, "runtime recreation after failure injection");
        if (rt) JS_FreeRuntime(rt);
        check(!control.live, "recreated runtime has no leaked allocations");
    }
done:
    if (negative_control) check(0, "expected negative control");
    printf("QUICKJS_RUNTIME checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
