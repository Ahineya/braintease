/* SPDX-License-Identifier: MIT
 * Host reference only until the complete engine passes the C16 port gates.
 * Uses the engine directly: no quickjs-libc, POSIX module, or qjs executable.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"

static unsigned checks, callbacks, interrupts, unhandled;

static void rejection(JSContext *ctx, JSValueConst promise, JSValueConst reason,
                       JS_BOOL handled, void *opaque)
{
    (void)ctx; (void)promise; (void)reason; (void)opaque;
    if (!handled) ++unhandled;
    else if (unhandled) --unhandled;
}

static void exception(JSContext *ctx)
{
    JSValue value = JS_GetException(ctx);
    const char *message = JS_ToCString(ctx, value);
    fprintf(stderr, "QuickJS exception: %s\n", message ? message : "<unprintable>");
    JS_FreeCString(ctx, message);
    JS_FreeValue(ctx, value);
}

static JSValue native_assert(JSContext *ctx, JSValueConst self,
                             int argc, JSValueConst *argv)
{
    const char *label;
    (void)self;
    ++callbacks;
    if (argc != 2 || JS_ToBool(ctx, argv[0]) != 1) {
        label = argc > 1 ? JS_ToCString(ctx, argv[1]) : NULL;
        JSValue error = JS_ThrowInternalError(ctx, "Assertion failed: %s", label ? label : "<missing>");
        JS_FreeCString(ctx, label);
        return error;
    }
    ++checks;
    return JS_UNDEFINED;
}

static JSValue native_add(JSContext *ctx, JSValueConst self,
                          int argc, JSValueConst *argv)
{
    int32_t a, b;
    (void)self;
    ++callbacks;
    if (argc != 2) return JS_ThrowTypeError(ctx, "nativeAdd expects two arguments");
    if (JS_ToInt32(ctx, &a, argv[0]) || JS_ToInt32(ctx, &b, argv[1]))
        return JS_EXCEPTION;
    /* The fixture uses positive values with a non-overflowing sum. */
    return JS_NewInt32(ctx, a + b);
}

static JSValue native_gc(JSContext *ctx, JSValueConst self,
                         int argc, JSValueConst *argv)
{
    (void)self; (void)argc; (void)argv;
    ++callbacks;
    JS_RunGC(JS_GetRuntime(ctx));
    return JS_UNDEFINED;
}

static int interrupt(JSRuntime *runtime, void *opaque)
{
    (void)runtime; (void)opaque;
    return ++interrupts >= 4;
}

static int eval_ok(JSContext *ctx, const char *source, int flags)
{
    JSValue value = JS_Eval(ctx, source, strlen(source), "<probe>", flags);
    int ok = !JS_IsException(value);
    if (!ok) exception(ctx);
    JS_FreeValue(ctx, value);
    return ok;
}

static int expect_exception(JSContext *ctx, const char *source)
{
    JSValue value = JS_Eval(ctx, source, strlen(source), "<expected-error>", JS_EVAL_TYPE_GLOBAL);
    int ok = JS_IsException(value);
    if (ok) {
        JSValue error = JS_GetException(ctx);
        JS_FreeValue(ctx, error);
        ++checks;
    }
    JS_FreeValue(ctx, value);
    return ok;
}

static int add_function(JSContext *ctx, JSValue global, const char *name,
                        JSCFunction *function, int argc)
{
    JSValue value = JS_NewCFunction(ctx, function, name, argc);
    if (JS_IsException(value)) return -1;
    return JS_SetPropertyStr(ctx, global, name, value);
}

int main(int argc, char **argv)
{
    JSRuntime *runtime;
    JSContext *ctx, *job_ctx;
    JSValue global;
    JSMemoryUsage memory;
    FILE *file;
    char *source;
    long length;
    int result = 1, jobs = 0, status;
    if (argc != 2) { fprintf(stderr, "Usage: %s semantics.js\n", argv[0]); return 2; }
    file = fopen(argv[1], "rb");
    if (!file) { perror(argv[1]); return 2; }
    if (fseek(file, 0, SEEK_END) || (length = ftell(file)) < 0 || length > 65536 ||
        fseek(file, 0, SEEK_SET)) { fclose(file); return 2; }
    source = malloc((size_t)length + 1);
    if (!source) { fclose(file); return 2; }
    if (fread(source, 1, (size_t)length, file) != (size_t)length) {
        fclose(file); free(source); return 2;
    }
    fclose(file); source[length] = 0;
    runtime = JS_NewRuntime();
    if (!runtime) { free(source); return 2; }
    JS_SetMemoryLimit(runtime, 1024 * 1024);
    JS_SetGCThreshold(runtime, 65536);
    JS_SetMaxStackSize(runtime, 128 * 1024);
    JS_SetHostPromiseRejectionTracker(runtime, rejection, NULL);
    ctx = JS_NewContext(runtime);
    if (!ctx) { JS_FreeRuntime(runtime); free(source); return 2; }
    global = JS_GetGlobalObject(ctx);
    if (add_function(ctx, global, "nativeAssert", native_assert, 2) < 0 ||
        add_function(ctx, global, "nativeAdd", native_add, 2) < 0 ||
        add_function(ctx, global, "nativeGC", native_gc, 0) < 0) {
        exception(ctx); goto done;
    }
    if (!eval_ok(ctx, source, JS_EVAL_TYPE_GLOBAL)) goto done;
    while ((status = JS_ExecutePendingJob(runtime, &job_ctx)) > 0) {
        if (++jobs > 1000) { fprintf(stderr, "Job queue did not settle\n"); goto done; }
    }
    if (status < 0) { exception(job_ctx); goto done; }
    if (unhandled) { fprintf(stderr, "Unhandled Promise rejection\n"); goto done; }
    if (!eval_ok(ctx, "nativeAssert(asyncDone, 'async completion')", JS_EVAL_TYPE_GLOBAL)) goto done;
    if (!eval_ok(ctx, "export const answer = 42; nativeAssert(answer === 42, 'ES module')",
                 JS_EVAL_TYPE_MODULE)) goto done;
    while ((status = JS_ExecutePendingJob(runtime, &job_ctx)) > 0) {
        if (++jobs > 1000) goto done;
    }
    if (status < 0) { exception(job_ctx); goto done; }
    if (unhandled) { fprintf(stderr, "Unhandled module rejection\n"); goto done; }
    if (!expect_exception(ctx, "new ArrayBuffer(4 * 1024 * 1024)")) goto done;
    if (!eval_ok(ctx, "nativeAssert(6 * 7 === 42, 'recovery after allocation failure')",
                 JS_EVAL_TYPE_GLOBAL)) goto done;
    JS_SetInterruptHandler(runtime, interrupt, NULL);
    status = expect_exception(ctx, "while (true) {}");
    JS_SetInterruptHandler(runtime, NULL, NULL);
    if (!status || interrupts < 4) goto done;
    if (!eval_ok(ctx, "nativeAssert(6 * 7 === 42, 'recovery after interruption')",
                 JS_EVAL_TYPE_GLOBAL)) goto done;
    JS_RunGC(runtime);
    JS_ComputeMemoryUsage(runtime, &memory);
    printf("QUICKJS_REFERENCE checks=%u jobs=%d callbacks=%u interrupts=%u memory=%lld allocations=%lld\n",
           checks, jobs, callbacks, interrupts,
           (long long)memory.memory_used_size, (long long)memory.malloc_count);
    result = 0;
done:
    JS_FreeValue(ctx, global);
    JS_FreeContext(ctx);
    JS_FreeRuntime(runtime);
    free(source);
    return result;
}
