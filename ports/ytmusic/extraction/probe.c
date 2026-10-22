/* SPDX-License-Identifier: MIT
 * Restricted fixture host: no JS filesystem, network, module loader or process
 * bindings. Both reference and guest execute the same upstream solver bundle.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include "quickjs.h"
#ifdef QJS_C16_H
#include "qjs-platform.h"
typedef long ProbeBool;
typedef struct { QJSAllocatorControl stats; size_t live; } ProbeAllocator;
static void *allocate(JSMallocState *s, size_t n) {
    ProbeAllocator *p = s->opaque; void *v = qjs_c16_malloc_functions.js_malloc(s, n);
    if (v) ++p->live; return v;
}
static void release(JSMallocState *s, void *v) {
    ProbeAllocator *p = s->opaque; if (v) --p->live;
    qjs_c16_malloc_functions.js_free(s, v);
}
static void *resize(JSMallocState *s, void *v, size_t n) {
    ProbeAllocator *p = s->opaque; void *r = qjs_c16_malloc_functions.js_realloc(s, v, n);
    if (!v && r) ++p->live; else if (v && !n) --p->live; return r;
}
static size_t usable(const void *p) { return qjs_c16_malloc_functions.js_malloc_usable_size(p); }
static const JSMallocFunctions allocator = { allocate, release, resize, usable };
#else
typedef int ProbeBool;
#endif
#define INPUT_LIMIT 8388608UL
#define MEMORY_LIMIT 536870912UL
static unsigned long polls;
static time_t started;
static void expired(int number) { (void)number; _exit(124); }
static JSValue phase(JSContext *ctx, JSValueConst self, int32_t argc, JSValueConst *argv) {
    const char *text; (void)self;
    if (argc != 1) return JS_ThrowTypeError(ctx, "one phase label required");
    text = JS_ToCString(ctx, argv[0]);
    if (!text) return JS_EXCEPTION;
    if (strcmp(text, "prepared") && strcmp(text, "compiled")) {
        JS_FreeCString(ctx, text); return JS_ThrowTypeError(ctx, "unknown phase label");
    }
    printf("EXTRACT_PHASE %s seconds=%ld polls=%lu\n", text, (long)(time(NULL) - started), polls);
    fflush(stdout); JS_FreeCString(ctx, text);
    return JS_UNDEFINED;
}
static ProbeBool interrupt(JSRuntime *rt, void *opaque) {
    (void)rt; (void)opaque;
    return ++polls > 2000000UL || time(NULL) - started > 600;
}
static char *read_input(const char *name, size_t *size) {
    FILE *f = fopen(name, "rb"); char *data; long length;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) || (length = ftell(f)) < 0 ||
        (unsigned long)length > INPUT_LIMIT || fseek(f, 0, SEEK_SET)) { fclose(f); return NULL; }
    *size = (size_t)length; data = malloc(*size + 1);
    if (!data) { fclose(f); return NULL; }
    if (fread(data, 1, *size, f) != *size || ferror(f)) { free(data); fclose(f); return NULL; }
    fclose(f); data[*size] = 0; return data;
}
static void exception(JSContext *ctx) {
    JSValue error = JS_GetException(ctx), stack;
    const char *text = JS_ToCString(ctx, error);
    fprintf(stderr, "EXTRACT_ERROR %s\n", text ? text : "exception conversion failed");
    JS_FreeCString(ctx, text);
    if (JS_IsObject(error)) {
        stack = JS_GetPropertyStr(ctx, error, "stack");
        if (JS_IsString(stack)) {
            text = JS_ToCString(ctx, stack);
            if (text) fprintf(stderr, "%s\n", text);
            JS_FreeCString(ctx, text);
        }
        JS_FreeValue(ctx, stack);
    }
    JS_FreeValue(ctx, error);
}
int main(int argc, char **argv) {
    JSRuntime *rt; JSContext *ctx; JSValue result, global, api, fn, args[4];
    const char *text; char *data; size_t size; int status = 1, negative = 0, cached = 0; unsigned i;
#ifdef QJS_C16_H
    ProbeAllocator control;
    memset(&control, 0, sizeof(control));
#endif
    if (argc < 4 || argc > 6) {
        fputs("usage: extract-probe BUNDLE PLAYER PLAYER_ID [--negative] [--prepared]\n", stderr); return 2;
    }
    for (i = 4; i < (unsigned)argc; ++i) {
        if (!strcmp(argv[i], "--negative") && !negative) negative = 1;
        else if (!strcmp(argv[i], "--prepared") && !cached) cached = 1;
        else return 2;
    }
    started = time(NULL);
    signal(SIGALRM, expired); alarm(630);
#ifdef QJS_C16_H
    rt = JS_NewRuntime2(&allocator, &control);
#else
    rt = JS_NewRuntime();
#endif
    if (!rt) return 1;
    JS_SetMemoryLimit(rt, MEMORY_LIMIT);
#ifdef QJS_C16_H
    JS_SetMaxStackSize(rt, QJS_C16_TERMINAL_STACK_CELLS);
#else
    JS_SetMaxStackSize(rt, 1048576UL);
#endif
    JS_SetInterruptHandler(rt, interrupt, NULL);
    ctx = JS_NewContext(rt);
    if (!ctx) { JS_FreeRuntime(rt); return 1; }
    global = JS_GetGlobalObject(ctx);
    result = JS_NewCFunction(ctx, phase, "extractionPhase", 1);
    if (JS_IsException(result) || JS_SetPropertyStr(ctx, global, "extractionPhase", result) < 0) {
        JS_FreeValue(ctx, global); exception(ctx); goto done;
    }
    JS_FreeValue(ctx, global);
    data = read_input(argv[1], &size);
    if (!data) { fputs("Cannot read bounded solver bundle\n", stderr); goto done; }
    result = JS_Eval(ctx, data, size, "<ejs-fixture>", JS_EVAL_TYPE_GLOBAL); free(data);
    if (JS_IsException(result)) { exception(ctx); goto done; }
    JS_FreeValue(ctx, result);
    data = read_input(argv[2], &size);
    if (!data) { fputs("Cannot read bounded player fixture\n", stderr); goto done; }
    args[0] = JS_NewStringLen(ctx, data, size); free(data);
    args[1] = JS_NewString(ctx, argv[3]); args[2] = JS_NewBool(ctx, negative); args[3] = JS_NewBool(ctx, cached);
    for (i = 0; i < 4; ++i) if (JS_IsException(args[i])) {
        unsigned j; for (j = 0; j < 4; ++j) JS_FreeValue(ctx, args[j]); exception(ctx); goto done;
    }
    global = JS_GetGlobalObject(ctx); api = JS_GetPropertyStr(ctx, global, "extractionProbe");
    fn = JS_GetPropertyStr(ctx, api, "run");
    puts("EXTRACT_BEGIN"); fflush(stdout);
    result = JS_Call(ctx, fn, api, 4, args);
    JS_FreeValue(ctx, fn); JS_FreeValue(ctx, api); JS_FreeValue(ctx, global);
    for (i = 0; i < 4; ++i) JS_FreeValue(ctx, args[i]);
    if (JS_IsException(result)) { exception(ctx); goto done; }
    text = JS_ToCString(ctx, result);
    if (text) { printf("EXTRACT_RESULT %s\n", text); status = ferror(stdout) ? 1 : 0; }
    else exception(ctx);
    JS_FreeCString(ctx, text); JS_FreeValue(ctx, result);
done:
    { JSMemoryUsage usage; JS_ComputeMemoryUsage(rt, &usage);
      printf("EXTRACT_MEMORY retained_units=%lu allocations=%lu\n", (unsigned long)usage.malloc_size, (unsigned long)usage.malloc_count); }
    JS_FreeContext(ctx); JS_FreeRuntime(rt);
    alarm(0);
#ifdef QJS_C16_H
    printf("EXTRACT_RESOURCES peak_cells=%lu polls=%lu live=%lu\n", (unsigned long)control.stats.peak_cells, polls, (unsigned long)control.live);
    if (control.live) status = 1;
#else
    printf("EXTRACT_RESOURCES polls=%lu\n", polls);
#endif
    return status;
}
