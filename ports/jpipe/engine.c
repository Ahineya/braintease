/* SPDX-License-Identifier: MIT */
#include "jpipe.h"
static const char *names[] = {"map", "filter", "reduce", "sort"};
static uint64_t now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
void j_budget(JPipe *p) { p->polls = 0; p->timed_out = 0; p->deadline = now() + p->time_limit; }
static JS_BOOL interrupt(JSRuntime *rt, void *opaque) {
    JPipe *p = opaque; (void)rt;
    if (++p->polls >= p->poll_limit || now() >= p->deadline) { p->timed_out = 1; return 1; }
    return 0;
}
int j_error(JPipe *p, const char *message) {
    if (!p->failed) {
        if (p->active < p->count)
            fprintf(stderr, "jpipe: stage %lu (%s), record %lu: %s\n", (unsigned long)p->active + 1,
                names[p->stages[p->active].kind], p->stages[p->active].records, message);
        else fprintf(stderr, "jpipe: input/output record %lu: %s\n", p->input_record, message);
    }
    p->failed = 1; return -1;
}
int j_exception(JPipe *p) {
    JSValue error = JS_GetException(p->ctx); const char *text;
    if (p->timed_out) j_error(p, "JavaScript execution budget exceeded");
    else {
        /* Reporting a user-defined exception must also remain interruptible. */
        j_budget(p); text = JS_ToCString(p->ctx, error);
        j_error(p, text ? text : "JavaScript error or out of memory");
        if (text) JS_FreeCString(p->ctx, text);
    }
    JS_FreeValue(p->ctx, error); return -1;
}
static JSValue diagnostic(JSContext *ctx, JSValueConst self, int32_t argc, JSValueConst *argv) {
    int32_t i; size_t length; const char *text; (void)self;
    for (i = 0; i < argc; i++) {
        text = JS_ToCStringLen(ctx, &length, argv[i]); if (!text) return JS_EXCEPTION;
        if (i) fputc(' ', stderr);
        fwrite(text, 1, length, stderr); JS_FreeCString(ctx, text);
    }
    fputc('\n', stderr); return JS_UNDEFINED;
}
static JSValue json_value(JSContext *ctx, JSValueConst self, int32_t argc, JSValueConst *argv) {
    JSValueConst v = argc > 1 ? argv[1] : JS_UNDEFINED; double number; (void)self;
    if (JS_IsUndefined(v) || JS_IsSymbol(v) || JS_IsBigInt(ctx, v) || JS_IsFunction(ctx, v))
        return JS_ThrowTypeError(ctx, "result is not JSON-compatible");
    if (JS_IsNumber(v)) {
        if (JS_ToFloat64(ctx, &number, v)) return JS_EXCEPTION;
        if (!isfinite(number)) return JS_ThrowTypeError(ctx, "non-finite result is not JSON-compatible");
    }
    return JS_DupValue(ctx, v);
}
static void rejection(JSContext *ctx, JSValueConst promise, JSValueConst reason, JS_BOOL handled, void *opaque) {
    JPipe *p = opaque; (void)ctx; (void)promise; (void)reason; (void)handled; p->async_used = 1;
}
static int synchronous(JPipe *p, JSValueConst v) {
    JSPromiseStateEnum state = JS_PromiseState(p->ctx, v);
    if (state == JS_PROMISE_PENDING || state == JS_PROMISE_FULFILLED || state == JS_PROMISE_REJECTED || JS_IsJobPending(p->rt) || p->async_used)
        return j_error(p, "async callbacks/Promise jobs are not supported yet");
    return 0;
}
int j_runtime(JPipe *p) {
    JSValue global, console, fn; size_t i, length; char *source;
#ifdef QJS_C16_H
    p->rt = JS_NewRuntime2(&qjs_c16_malloc_functions, NULL);
#else
    p->rt = JS_NewRuntime();
#endif
    if (!p->rt) return j_error(p, "cannot create JavaScript runtime");
    JS_SetMemoryLimit(p->rt, p->memory);
#ifdef QJS_C16_H
    JS_SetMaxStackSize(p->rt, QJS_C16_TERMINAL_STACK_CELLS);
#else
    JS_SetMaxStackSize(p->rt, 512 * 1024);
#endif
    p->ctx = JS_NewContext(p->rt);
    if (!p->ctx) return j_error(p, "cannot create context within --memory budget");
    JS_SetInterruptHandler(p->rt, interrupt, p);
    JS_SetHostPromiseRejectionTracker(p->rt, rejection, p);
    j_budget(p);
    global = JS_GetGlobalObject(p->ctx); console = JS_NewObject(p->ctx);
    if (JS_IsException(console)) { JS_FreeValue(p->ctx, global); return j_exception(p); }
    for (i = 0; i < 5; i++) {
        const char *method = i == 0 ? "log" : i == 1 ? "error" : i == 2 ? "warn" : i == 3 ? "info" : "debug";
        fn = JS_NewCFunction(p->ctx, diagnostic, method, 1);
        if (JS_IsException(fn) || JS_SetPropertyStr(p->ctx, console, method, fn) < 0) {
            JS_FreeValue(p->ctx, console); JS_FreeValue(p->ctx, global); return j_exception(p);
        }
    }
    if (JS_SetPropertyStr(p->ctx, global, "console", console) < 0) { JS_FreeValue(p->ctx, global); return j_exception(p); }
    fn = JS_NewCFunction(p->ctx, diagnostic, "print", 1);
    if (JS_SetPropertyStr(p->ctx, global, "print", fn) < 0) { JS_FreeValue(p->ctx, global); return j_exception(p); }
    JS_FreeValue(p->ctx, global);
    p->replacer = JS_NewCFunction(p->ctx, json_value, "jsonValue", 2);
    if (JS_IsException(p->replacer)) return j_exception(p);
    for (i = 0; i < p->count; i++) {
        JStage *s = &p->stages[i]; p->active = i;
        length = strlen(s->source);
        if (length > (size_t)-1 - 4) return j_error(p, "function source too large");
        source = js_malloc(p->ctx, length + 4); if (!source) return j_exception(p);
        source[0] = '('; memcpy(source + 1, s->source, length); memcpy(source + 1 + length, "\n)", 3);
        j_budget(p); s->function = JS_Eval(p->ctx, source, length + 3, "<pipeline-function>", JS_EVAL_TYPE_GLOBAL);
        js_free(p->ctx, source);
        if (JS_IsException(s->function)) return j_exception(p);
        if (synchronous(p, s->function)) return -1;
        if (!JS_IsFunction(p->ctx, s->function)) return j_error(p, "expression must evaluate to a function");
        if (s->kind == J_REDUCE) {
            j_budget(p); s->accumulator = JS_ParseJSON(p->ctx, s->seed, strlen(s->seed), "<initial-value>");
            if (JS_IsException(s->accumulator)) return j_exception(p);
        }
    }
    p->active = p->count; return 0;
}
static int output(JPipe *p, JSValueConst value) {
    JSValue encoded = JS_UNDEFINED; const char *text; size_t length; int bad;
    p->active = p->count; j_budget(p);
    if (p->output == J_TEXT) {
        if (!JS_IsString(value)) return j_error(p, "text output requires strings (convert explicitly in map)");
        text = JS_ToCStringLen(p->ctx, &length, value);
    } else {
        encoded = JS_JSONStringify(p->ctx, value, p->replacer, JS_UNDEFINED);
        if (JS_IsException(encoded)) return j_exception(p);
        if (JS_IsUndefined(encoded)) return j_error(p, "result is not JSON-compatible");
        text = JS_ToCStringLen(p->ctx, &length, encoded);
    }
    if (!text) { JS_FreeValue(p->ctx, encoded); return j_exception(p); }
    if (synchronous(p, value)) { JS_FreeCString(p->ctx, text); JS_FreeValue(p->ctx, encoded); return -1; }
    if (length > p->record_limit) { JS_FreeCString(p->ctx, text); JS_FreeValue(p->ctx, encoded); return j_error(p, "output exceeds --max-record"); }
    bad = fwrite(text, 1, length, stdout) != length || fputc('\n', stdout) == EOF || fflush(stdout);
    JS_FreeCString(p->ctx, text); JS_FreeValue(p->ctx, encoded);
    if (bad) return j_error(p, "stdout write failed");
    return 0;
}
int j_push(JPipe *p, size_t start, JSValue value) {
    size_t i; JSValue result, args[3]; int keep;
    for (i = start; i < p->count; i++) {
        JStage *s = &p->stages[i]; p->active = i;
        if (++s->records == 0) { j_error(p, "record counter overflow"); goto fail; }
        if (s->kind == J_SORT) {
            if (s->used == s->capacity) {
                size_t n = s->capacity ? s->capacity * 2 : 16; JSValue *fresh;
                if (n < s->capacity || n > (size_t)-1 / sizeof(*fresh)) { j_error(p, "sort allocation overflow"); goto fail; }
                fresh = js_realloc(p->ctx, s->values, n * sizeof(*fresh));
                if (!fresh) { j_exception(p); goto fail; }
                s->values = fresh; s->capacity = n;
            }
            s->values[s->used++] = value; return 0;
        }
        args[0] = s->kind == J_REDUCE ? s->accumulator : value;
        args[1] = s->kind == J_REDUCE ? value : JS_NewFloat64(p->ctx, (double)(s->records - 1));
        args[2] = JS_NewFloat64(p->ctx, (double)(s->records - 1));
        j_budget(p); result = JS_Call(p->ctx, s->function, JS_UNDEFINED, s->kind == J_REDUCE ? 3 : 2, args);
        if (JS_IsException(result)) { j_exception(p); goto fail; }
        if (synchronous(p, result)) { JS_FreeValue(p->ctx, result); goto fail; }
        if (s->kind == J_FILTER) {
            keep = JS_ToBool(p->ctx, result); JS_FreeValue(p->ctx, result);
            if (keep < 0) { j_exception(p); goto fail; }
            if (!keep) { JS_FreeValue(p->ctx, value); return 0; }
        } else if (s->kind == J_REDUCE) {
            JS_FreeValue(p->ctx, s->accumulator); s->accumulator = result;
            JS_FreeValue(p->ctx, value); return 0;
        } else { JS_FreeValue(p->ctx, value); value = result; }
    }
    keep = output(p, value); JS_FreeValue(p->ctx, value); return keep;
fail:
    JS_FreeValue(p->ctx, value); return -1;
}
static int compare(JPipe *p, JStage *s, JSValueConst a, JSValueConst b, int *order) {
    JSValue args[2], result; double number; int bad;
    args[0] = a; args[1] = b; j_budget(p);
    result = JS_Call(p->ctx, s->function, JS_UNDEFINED, 2, args);
    if (JS_IsException(result)) return j_exception(p);
    bad = synchronous(p, result);
    if (!bad && (!JS_IsNumber(result) || JS_ToFloat64(p->ctx, &number, result) || !isfinite(number)))
        bad = j_error(p, "sort comparator must return a finite number");
    JS_FreeValue(p->ctx, result); if (bad) return -1;
    *order = number < 0 ? -1 : number > 0 ? 1 : 0; return 0;
}
/* Stable bottom-up merge sort. Scratch slots borrow values: only the primary
 * vector owns them, even if a JS comparator throws partway through a pass. */
static int sort(JPipe *p, JStage *s) {
    size_t width, base, middle, end, a, b, at, n = s->used; int order; JSValue *scratch;
    if (n < 2) return 0;
    scratch = js_malloc(p->ctx, n * sizeof(*scratch)); if (!scratch) return j_exception(p);
    for (width = 1; width < n;) {
        for (base = 0; base < n; base = end) {
            middle = base + (n-base < width ? n-base : width);
            end = middle + (n-middle < width ? n-middle : width);
            a = base; b = middle;
            for (at = base; at < end; at++) {
                order = 0;
                if (a < middle && b < end && compare(p, s, s->values[a], s->values[b], &order)) { js_free(p->ctx, scratch); return -1; }
                scratch[at] = b == end || (a < middle && order <= 0) ? s->values[a++] : s->values[b++];
            }
        }
        memcpy(s->values, scratch, n * sizeof(*scratch));
        if (width > n / 2) break;
        width *= 2;
    }
    js_free(p->ctx, scratch); return 0;
}
int j_finish(JPipe *p) {
    size_t i, j; JSValue value;
    for (i = 0; i < p->count; i++) {
        JStage *s = &p->stages[i]; p->active = i;
        if (s->kind == J_REDUCE) {
            value = s->accumulator; s->accumulator = JS_UNDEFINED;
            if (j_push(p, i+1, value)) return -1;
        } else if (s->kind == J_SORT) {
            if (sort(p, s)) return -1;
            for (j = 0; j < s->used; j++) {
                value = s->values[j]; s->values[j] = JS_UNDEFINED;
                if (j_push(p, i+1, value)) return -1;
            }
            js_free(p->ctx, s->values); s->values = NULL; s->used = s->capacity = 0;
        }
    }
    return 0;
}
void j_dispose(JPipe *p) {
    size_t i, j;
    if (p->ctx) {
        for (i = 0; i < p->count; i++) {
            JStage *s = &p->stages[i];
            JS_FreeValue(p->ctx, s->function); JS_FreeValue(p->ctx, s->accumulator);
            for (j = 0; j < s->used; j++) JS_FreeValue(p->ctx, s->values[j]);
            js_free(p->ctx, s->values);
        }
        JS_FreeValue(p->ctx, p->replacer); JS_FreeContext(p->ctx);
    }
    if (p->rt) JS_FreeRuntime(p->rt);
    free(p->stages);
}
