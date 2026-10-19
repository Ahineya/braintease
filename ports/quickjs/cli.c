/* SPDX-License-Identifier: MIT
 * Small portable QuickJS command host. No quickjs-libc/Node/OS module ABI.
 */
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "quickjs.h"
#include "system.h"
#ifdef QJS_C16_H
#include "qjs-platform.h"
#endif

#define SOURCE_LIMIT 1048576UL
#define DEFAULT_MEMORY 1048576UL
#define DEFAULT_POLLS 10000UL
#define DEFAULT_JOBS 1024UL
#define DEFAULT_TIME 60000UL
static volatile sig_atomic_t stop_signal;
static void stop_requested(int signal_number) { stop_signal = signal_number; }
typedef struct Rejection {
    JSValue promise, reason;
    struct Rejection *next;
} Rejection;
typedef struct {
    unsigned long polls, poll_limit, job_limit;
    Rejection *rejections;
    int tracker_failed;
    QJSSystem *system;
    uint64_t deadline;
    unsigned long time_limit;
    int timed_out, server;
} Host;

static void discard_exception(JSContext *ctx)
{
    JSValue value = JS_GetException(ctx);
    JS_FreeValue(ctx, value);
}

static int write_value(JSContext *ctx, JSValueConst value, FILE *stream)
{
    size_t length;
    const char *text = JS_ToCStringLen(ctx, &length, value);
    int failed;
    if (!text) return -1;
    failed = fwrite(text, 1, length, stream) != length;
    JS_FreeCString(ctx, text);
    if (failed) {
        JS_ThrowInternalError(ctx, "output write failed");
        return -1;
    }
    return 0;
}

static void report_value(JSContext *ctx, JSValueConst value)
{
    JSValue stack;
    if (write_value(ctx, value, stderr)) {
        discard_exception(ctx);
        fputs("<unprintable exception>", stderr);
    }
    fputc('\n', stderr);
    if (JS_IsError(ctx, value)) {
        stack = JS_GetPropertyStr(ctx, value, "stack");
        if (JS_IsException(stack)) discard_exception(ctx);
        else if (JS_IsString(stack) && write_value(ctx, stack, stderr)) discard_exception(ctx);
        JS_FreeValue(ctx, stack);
    }
}

static void report_exception(JSContext *ctx)
{
    JSValue value = JS_GetException(ctx);
    report_value(ctx, value);
    JS_FreeValue(ctx, value);
}

static JSValue console_write(JSContext *ctx, JSValueConst self, int32_t argc,
                             JSValueConst *argv, int32_t error_stream)
{
    int32_t i;
    FILE *stream = error_stream ? stderr : stdout;
    (void)self;
    for (i = 0; i < argc; ++i) {
        if (i && fputc(' ', stream) == EOF) goto fail;
        if (write_value(ctx, argv[i], stream)) return JS_EXCEPTION;
    }
    if (fputc('\n', stream) == EOF || fflush(stream)) goto fail;
    return JS_UNDEFINED;
fail:
    return JS_ThrowInternalError(ctx, "output write failed");
}

static JS_BOOL interrupted(JSRuntime *rt, void *opaque)
{
    Host *host = opaque;
    uint64_t now;
    (void)rt;
    if (stop_signal) return 1;
    if (++host->polls >= host->poll_limit) return 1;
    if (host->deadline && !qjs_system_now(&now) && now >= host->deadline) {
        host->timed_out = 1;
        return 1;
    }
    return 0;
}

static void evaluation_exception(JSContext *ctx, Host *host)
{
    if (stop_signal) {
        discard_exception(ctx);
        fputs("js: interrupted by signal\n", stderr);
    } else if (host->timed_out) {
        discard_exception(ctx);
        fputs("js: event-loop time budget exceeded\n", stderr);
    } else report_exception(ctx);
}

static void rejection(JSContext *ctx, JSValueConst promise, JSValueConst reason,
                       JS_BOOL handled, void *opaque)
{
    Host *host = opaque;
    Rejection **at = &host->rejections, *entry;
    while (*at && !JS_StrictEq(ctx, (*at)->promise, promise)) at = &(*at)->next;
    if (handled) {
        if (*at) {
            entry = *at; *at = entry->next;
            JS_FreeValue(ctx, entry->promise); JS_FreeValue(ctx, entry->reason);
            js_free(ctx, entry);
        }
    } else if (!*at) {
        /* Runtime allocation does not throw from this observer callback. */
        entry = js_malloc_rt(JS_GetRuntime(ctx), sizeof(*entry));
        if (!entry) { host->tracker_failed = 1; return; }
        entry->promise = JS_DupValue(ctx, promise);
        entry->reason = JS_DupValue(ctx, reason);
        entry->next = NULL; *at = entry;
    }
}

static void clear_rejections(JSContext *ctx, Host *host)
{
    Rejection *entry;
    while (host->rejections) {
        entry = host->rejections; host->rejections = entry->next;
        JS_FreeValue(ctx, entry->promise); JS_FreeValue(ctx, entry->reason);
        js_free(ctx, entry);
    }
    host->tracker_failed = 0;
}

static char *read_source(FILE *file, size_t *length)
{
    size_t size = 0, capacity = 4096, count, i;
    char *source = malloc(capacity + 1), *fresh;
    if (!source) return NULL;
    for (;;) {
        if (stop_signal) { errno = EINTR; goto fail; }
        if (size == capacity) {
            if (capacity == SOURCE_LIMIT) {
                if (fgetc(file) == EOF && !ferror(file)) break;
                errno = EFBIG; goto fail;
            }
            capacity *= 2;
            fresh = realloc(source, capacity + 1);
            if (!fresh) goto fail;
            source = fresh;
        }
        count = fread(source + size, 1, capacity - size, file);
        size += count;
        if (!count) {
            if (ferror(file)) goto fail;
            break;
        }
    }
    for (i = 0; i < size; ++i) {
        if (!source[i]
#if CHAR_BIT > 8
            || (unsigned char)source[i] > 255U
#endif
        ) { errno = EINVAL; goto fail; }
    }
    source[size] = 0; *length = size;
    return source;
fail:
    free(source); return NULL;
}

static JSModuleDef *load_module(JSContext *ctx, const char *name, void *opaque)
{
    FILE *file;
    char *source;
    size_t length;
    JSValue compiled;
    JSModuleDef *module;
    (void)opaque;
    if (!strcmp(name, "fuse:http") || !strcmp(name, "fuse:net") ||
        !strcmp(name, "fuse:http-server")) return qjs_system_module(ctx, name);
    file = fopen(name, "rb");
    if (!file) {
        JS_ThrowReferenceError(ctx, "cannot open module '%s': %s", name, strerror(errno));
        return NULL;
    }
    source = read_source(file, &length); fclose(file);
    if (!source) {
        JS_ThrowReferenceError(ctx, "cannot read module '%s'", name); return NULL;
    }
    compiled = JS_Eval(ctx, source, length, name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    free(source);
    if (JS_IsException(compiled)) return NULL;
    module = JS_VALUE_GET_PTR(compiled);
    JS_FreeValue(ctx, compiled); /* retained by the context's module registry */
    return module;
}

static int set_property(JSContext *ctx, JSValueConst object, const char *name, JSValue value)
{
    if (JS_IsException(value)) return -1;
    return JS_SetPropertyStr(ctx, object, name, value) < 0 ? -1 : 0;
}

static int helpers(JSContext *ctx, int argc, char **argv, const char *filename)
{
    JSValue global = JS_GetGlobalObject(ctx), console = JS_NewObject(ctx), args = JS_NewArray(ctx);
    JSValue log, value;
    int i, ok = 0;
    if (JS_IsException(console) || JS_IsException(args)) goto done;
    log = JS_NewCFunctionMagic(ctx, console_write, "log", 1, JS_CFUNC_generic_magic, 0);
    if (JS_IsException(log)) goto done;
    if (set_property(ctx, global, "print", JS_DupValue(ctx, log))) { JS_FreeValue(ctx, log); goto done; }
    if (set_property(ctx, console, "log", log) ||
        set_property(ctx, console, "error", JS_NewCFunctionMagic(ctx, console_write, "error", 1, JS_CFUNC_generic_magic, 1)))
        goto done;
    value = JS_NewString(ctx, filename);
    if (JS_IsException(value) || JS_SetPropertyUint32(ctx, args, 0, value) < 0) goto done;
    for (i = 0; i < argc; ++i) {
        value = JS_NewString(ctx, argv[i]);
        if (JS_IsException(value) || JS_SetPropertyUint32(ctx, args, (uint32_t)i + 1, value) < 0) goto done;
    }
    if (set_property(ctx, global, "console", JS_DupValue(ctx, console)) ||
        set_property(ctx, global, "scriptArgs", JS_DupValue(ctx, args))) goto done;
    ok = 1;
done:
    JS_FreeValue(ctx, console); JS_FreeValue(ctx, args); JS_FreeValue(ctx, global);
    return ok ? 0 : -1;
}

static int evaluate(JSContext *ctx, Host *host, const char *source, size_t length,
                    const char *filename, int module, int display)
{
    JSValue result, reason;
    JSContext *job_ctx;
    Rejection *entry;
    unsigned long jobs = 0;
    int32_t rc = 0, batch;
    int status = 0;
    uint64_t now;
    const char *display_text = NULL;
    size_t display_length = 0;
    host->polls = 0;
    host->timed_out = 0;
    if (qjs_system_now(&now)) { fputs("js: monotonic clock failed\n", stderr); return 1; }
    host->deadline = host->time_limit ? now + host->time_limit : 0;
    result = JS_Eval(ctx, source, length, filename, module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) { evaluation_exception(ctx, host); status = 1; goto done; }
    /* Conversion can execute user code, enqueue timers or reject Promises.
     * Do it before draining work; emit the saved octets after the loop. */
    if (display && !JS_IsUndefined(result)) {
        display_text = JS_ToCStringLen(ctx, &display_length, result);
        if (!display_text) { evaluation_exception(ctx, host); status = 1; goto done; }
    }
    for (;;) {
      if (stop_signal) {
          fputs("js: interrupted by signal\n", stderr); status = 1; goto done;
      }
      /* Yield to ready OS work between bounded microtask batches. Neither a
       * Promise chain nor a busy file/pipe may starve timers indefinitely. */
      for (batch = 0; batch < 32 && JS_IsJobPending(JS_GetRuntime(ctx)); ++batch) {
        if (host->job_limit && jobs++ >= host->job_limit) {
            fputs("js: pending-job budget exceeded\n", stderr); status = 1; goto done;
        }
        if (host->server) host->polls = 0;
        rc = JS_ExecutePendingJob(JS_GetRuntime(ctx), &job_ctx);
        if (rc < 0) { evaluation_exception(job_ctx, host); status = 1; goto done; }
      }
      if (!JS_IsJobPending(JS_GetRuntime(ctx)) && !qjs_system_pending(host->system)) break;
      if (qjs_system_now(&now) || (host->deadline && now >= host->deadline)) {
          fputs("js: event-loop time budget exceeded\n", stderr); status = 1; goto done;
      }
      if (host->server) host->polls = 0;
      if (qjs_system_step(host->system, JS_IsJobPending(JS_GetRuntime(ctx)) ? 0 :
          (host->deadline ? (unsigned long)(host->deadline - now) : 1000)) < 0) {
          evaluation_exception(ctx, host); status = 1; goto done;
      }
    }
    if (module && JS_PromiseState(ctx, result) == JS_PROMISE_PENDING) {
        fputs("js: top-level await did not settle\n", stderr); status = 1;
    } else if (module && JS_PromiseState(ctx, result) == JS_PROMISE_REJECTED) {
        reason = JS_PromiseResult(ctx, result);
        report_value(ctx, reason); JS_FreeValue(ctx, reason); status = 1;
    }
    for (entry = host->rejections; entry; entry = entry->next) {
        if (!module || !JS_StrictEq(ctx, result, entry->promise)) {
            fputs("Unhandled rejection: ", stderr); report_value(ctx, entry->reason);
        }
        status = 1;
    }
    if (host->tracker_failed) { fputs("js: rejection tracking allocation failed\n", stderr); status = 1; }
    if (!status && display_text) {
        if (fwrite(display_text, 1, display_length, stdout) != display_length ||
            fputc('\n', stdout) == EOF || fflush(stdout)) {
            fputs("js: output write failed\n", stderr); status = 1;
        }
    }
done:
    host->deadline = 0;
    if (display_text) JS_FreeCString(ctx, display_text);
    if (status) qjs_system_cancel(host->system);
    JS_FreeValue(ctx, result); clear_rejections(ctx, host);
    return status;
}

static int repl(JSContext *ctx, Host *host)
{
    char line[4096];
    int status = 0;
    puts("QuickJS / C16 terminal host. One expression or statement per line; .exit quits.");
    for (;;) {
        fputs("js> ", stdout); fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        if (!strcmp(line, ".exit\n") || !strcmp(line, ".exit")) break;
        if (!strchr(line, '\n') && !feof(stdin)) {
            int ch;
            while ((ch = fgetc(stdin)) != '\n' && ch != EOF) {}
            fputs("js: input line too long\n", stderr); status = 1; continue;
        }
        if (evaluate(ctx, host, line, strlen(line), "<repl>", 0, 1)) status = 1;
        if (stop_signal) break;
        /* A budget-exhausted job chain cannot safely carry into another turn. */
        if (JS_IsJobPending(JS_GetRuntime(ctx))) break;
    }
    return ferror(stdin) ? 1 : status;
}

static int number(const char *arg, unsigned long *value)
{
    char *end;
    unsigned long parsed;
    if (!arg || *arg < '0' || *arg > '9') return -1;
    errno = 0; parsed = strtoul(arg, &end, 10);
    if (errno || *end || !parsed) return -1;
    *value = parsed; return 0;
}

static void usage(FILE *stream)
{
    fputs("Usage: js [options] [file|- [args...]]\n"
          "  -e code       execute JavaScript\n"
          "  -p code       execute and print the result\n"
          "  -m, --module  evaluate as an ES module\n"
          "  --server      long-running mode; retain per-turn CPU and memory limits\n"
          "  -i            enter the line REPL after execution\n"
          "  --memory n    engine allocation limit (C16 cells on guests)\n"
          "  --max-polls n interrupt callback budget per evaluation\n"
          "  --max-jobs n  pending Promise job budget\n"
          "  --max-time-ms n wall-clock budget per evaluation (default 60000)\n"
          "  --help        show this help\n"
          "  --version     show engine version\n", stream);
}

int main(int argc, char **argv)
{
    JSRuntime *rt;
    JSContext *ctx;
    Host host;
    unsigned long memory = DEFAULT_MEMORY;
    const char *expression = NULL, *filename = "<stdin>";
    char *source = NULL;
    size_t length = 0;
    FILE *file = NULL;
    int i, module = 0, display = 0, interactive = 0, status = 0;
    void (*saved_int)(int), (*saved_term)(int);
#ifdef QJS_C16_H
    QJSAllocatorControl control;
    memset(&control, 0, sizeof(control));
#endif
    memset(&host, 0, sizeof(host)); host.poll_limit = DEFAULT_POLLS; host.job_limit = DEFAULT_JOBS;
    host.time_limit = DEFAULT_TIME;
    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (!strcmp(arg, "--")) { ++i; break; }
        if (*arg != '-' || !strcmp(arg, "-")) break;
        if (!strcmp(arg, "--help")) { usage(stdout); return 0; }
        if (!strcmp(arg, "--version")) { puts("QuickJS 2026-06-04 / portable terminal host"); return 0; }
        if (!strcmp(arg, "--server")) { host.server = 1; host.time_limit = 0; host.job_limit = 0; }
        else if (!strcmp(arg, "-m") || !strcmp(arg, "--module")) module = 1;
        else if (!strcmp(arg, "-i")) interactive = 1;
        else if (!strcmp(arg, "-e") || !strcmp(arg, "-p")) {
            display = !strcmp(arg, "-p");
            if (++i == argc) goto bad_option;
            expression = argv[i++]; filename = "<eval>"; break;
        } else if (!strcmp(arg, "--memory") || !strcmp(arg, "--max-polls") || !strcmp(arg, "--max-jobs") || !strcmp(arg, "--max-time-ms")) {
            unsigned long *out = !strcmp(arg, "--memory") ? &memory : !strcmp(arg, "--max-polls") ? &host.poll_limit : !strcmp(arg, "--max-jobs") ? &host.job_limit : &host.time_limit;
            if (++i == argc || number(argv[i], out)) goto bad_option;
        } else goto bad_option;
    }
    if (expression) { source = (char *)expression; length = strlen(expression); }
    else if (i < argc) {
        filename = argv[i++];
        file = !strcmp(filename, "-") ? stdin : fopen(filename, "rb");
        if (!file) { fprintf(stderr, "js: %s: %s\n", filename, strerror(errno)); return 1; }
    } else if (interactive || isatty(STDIN_FILENO)) interactive = 1;
    else file = stdin;
    if (file) {
        source = read_source(file, &length);
        if (file != stdin) fclose(file);
        if (!source) { fprintf(stderr, "js: cannot read %s: %s\n", filename, strerror(errno)); return 1; }
    }
    saved_int = signal(SIGINT, stop_requested);
    saved_term = signal(SIGTERM, stop_requested);
#ifdef QJS_C16_H
    rt = JS_NewRuntime2(&qjs_c16_malloc_functions, &control);
#else
    rt = JS_NewRuntime();
#endif
    if (!rt) { fputs("js: cannot create runtime\n", stderr); status = 1; goto source_done; }
    JS_SetMemoryLimit(rt, memory);
#ifdef QJS_C16_H
    JS_SetMaxStackSize(rt, QJS_C16_TERMINAL_STACK_CELLS);
#endif
    JS_SetHostPromiseRejectionTracker(rt, rejection, &host);
    JS_SetInterruptHandler(rt, interrupted, &host);
    JS_SetModuleLoaderFunc(rt, NULL, load_module, NULL);
    ctx = JS_NewContext(rt);
    if (!ctx) { fputs("js: cannot create context within memory limit\n", stderr); status = 1; goto runtime_done; }
    if (helpers(ctx, argc - i, argv + i, filename)) { report_exception(ctx); status = 1; goto context_done; }
    host.system = qjs_system_new(ctx);
    if (!host.system) { report_exception(ctx); status = 1; goto context_done; }
    if (source) {
        module = module || (strlen(filename) >= 4 && !strcmp(filename + strlen(filename) - 4, ".mjs")) || JS_DetectModule(source, length);
        status = evaluate(ctx, &host, source, length, filename, module, display);
    }
    if (interactive && !stop_signal && !JS_IsJobPending(rt)) status |= repl(ctx, &host);
context_done:
    qjs_system_free(host.system);
    JS_SetHostPromiseRejectionTracker(rt, NULL, NULL);
    clear_rejections(ctx, &host);
    JS_FreeContext(ctx);
runtime_done:
    JS_FreeRuntime(rt);
source_done:
    if (!expression) free(source);
    signal(SIGINT, saved_int); signal(SIGTERM, saved_term);
    if (stop_signal) status = 128 + stop_signal;
    return status;
bad_option:
    usage(stderr); return 2;
}
