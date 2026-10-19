/* SPDX-License-Identifier: MIT */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "system.h"
#include "qjs-http.inc"

#define TASKS 32
#define CHUNK 2048
#define TEXT_LIMIT 1048576UL
#define OUTPUT_LIMIT 65536UL
#define ENTRIES_LIMIT 4096UL
#define ARG_LIMIT 32
#define PATH_LIMIT 511
#define RUN_TIMEOUT 30000UL

enum Operation { READ_TEXT, READ_DIR, STAT_PATH, WRITE_TEXT, SLEEP, TIMER, RUN, NET_ACCEPT, NET_READ, NET_WRITE, NET_CONNECT };
typedef struct { char *data; size_t size, capacity; } Buffer;
typedef struct Task {
    enum Operation op;
    JSValue resolve, reject, value, callback;
    JSValue *arguments;
    int32_t argc;
    char *path, **argv;
    int fd, streams[3], status, reaped, failure;
    char exec_error[sizeof(int)];
    size_t exec_size;
    pid_t pid;
    DIR *directory;
    Buffer data, output, error;
    uint32_t entries, id, socket_id;
    size_t output_limit;
    int binary, cancelled;
    uint64_t due;
    uint64_t sequence;
} Task;
typedef struct { uint32_t id; int fd, listener, reading, writing; } Socket;
struct QJSSystem {
    JSContext *ctx;
    Task *tasks[TASKS];
    Socket sockets[TASKS];
    uint32_t next_socket;
    unsigned cursor;
    uint32_t next_id;
    uint64_t next_sequence;
};

int qjs_system_now(uint64_t *milliseconds)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return -1;
    *milliseconds = (uint64_t)t.tv_sec * 1000UL + (unsigned long)t.tv_nsec / 1000000UL;
    return 0;
}

/* Property setters consume their value, but an allocation exception must
 * remain pending rather than become a stored JS_EXCEPTION sentinel. */
static int property(JSContext *ctx, JSValueConst object, const char *name, JSValue value)
{
    if (JS_IsException(value)) return -1;
    return JS_SetPropertyStr(ctx, object, name, value);
}

static JSValue os_error(JSContext *ctx, int error, const char *operation, const char *path)
{
    JSValue value = JS_NewError(ctx);
    if (JS_IsException(value)) return value;
    if (property(ctx, value, "message", JS_NewString(ctx, strerror(error))) < 0 ||
        property(ctx, value, "errno", JS_NewInt32(ctx, error)) < 0 ||
        property(ctx, value, "syscall", JS_NewString(ctx, operation)) < 0 ||
        (path && property(ctx, value, "path", JS_NewString(ctx, path)) < 0)) {
        JS_FreeValue(ctx, value); return JS_EXCEPTION;
    }
    return value;
}

static int fail_loop(QJSSystem *s, int error, const char *operation)
{
    JSValue value = os_error(s->ctx, error, operation, NULL);
    if (!JS_IsException(value)) JS_Throw(s->ctx, value);
    return -1;
}

static void kill_child(Task *t)
{
    if (t->pid > 0) {
        /* Each launched program owns a group, so a timed-out shell cannot
         * leave descendants holding the capture pipes open indefinitely. */
        kill(-t->pid, SIGKILL);
        if (!t->reaped) kill(t->pid, SIGKILL);
    }
}

static Socket *socket_find(QJSSystem *s, uint32_t id)
{
    unsigned i;
    for (i = 0; i < TASKS; ++i) if (id && s->sockets[i].id == id) return &s->sockets[i];
    return NULL;
}
static void socket_drop(Socket *socket)
{
    if (!socket) return;
    close(socket->fd); memset(socket, 0, sizeof(*socket));
}
static void task_free(QJSSystem *s, Task *t)
{
    JSContext *ctx = s->ctx;
    int i, saved;
    if (!t) return;
    if (t->socket_id) {
        Socket *socket = socket_find(s, t->socket_id);
        if (socket) {
            if (t->op == NET_CONNECT && t->entries != 2) socket_drop(socket);
            else if (t->op == NET_WRITE || t->op == NET_CONNECT) socket->writing = 0;
            else socket->reading = 0;
        }
    }
    if (t->fd >= 0) close(t->fd);
    for (i = 0; i < 3; ++i) if (t->streams[i] >= 0) close(t->streams[i]);
    if (t->directory) closedir(t->directory);
    if (t->pid > 0 && !t->reaped) {
        saved = errno;
        kill_child(t);
        while (waitpid(t->pid, &t->status, 0) < 0 && errno == EINTR) {}
        errno = saved;
    }
    if (t->argv) {
        for (i = 0; t->argv[i]; ++i) js_free(ctx, t->argv[i]);
        js_free(ctx, t->argv);
    }
    for (i = 0; i < t->argc; ++i) JS_FreeValue(ctx, t->arguments[i]);
    js_free(ctx, t->arguments);
    js_free(ctx, t->path);
    js_free(ctx, t->data.data); js_free(ctx, t->output.data); js_free(ctx, t->error.data);
    JS_FreeValue(ctx, t->resolve); JS_FreeValue(ctx, t->reject);
    JS_FreeValue(ctx, t->value); JS_FreeValue(ctx, t->callback);
    js_free(ctx, t);
}

void qjs_system_cancel(QJSSystem *s)
{
    unsigned i;
    if (!s) return;
    for (i = 0; i < TASKS; ++i) {
        if (s->tasks[i] && s->tasks[i]->op == RUN) kill_child(s->tasks[i]);
        task_free(s, s->tasks[i]); s->tasks[i] = NULL;
    }
    for (i = 0; i < TASKS; ++i) if (s->sockets[i].id) socket_drop(&s->sockets[i]);
}

void qjs_system_free(QJSSystem *s)
{
    if (!s) return;
    qjs_system_cancel(s);
    JS_SetContextOpaque(s->ctx, NULL);
    js_free(s->ctx, s);
}

int qjs_system_pending(QJSSystem *s)
{
    unsigned i;
    for (i = 0; i < TASKS; ++i) if (s->tasks[i]) return 1;
    return 0;
}

static Task *task_new(JSContext *ctx, enum Operation op)
{
    Task *t = js_mallocz(ctx, sizeof(*t));
    int i;
    if (!t) return NULL;
    t->op = op; t->fd = -1; t->output_limit = OUTPUT_LIMIT;
    for (i = 0; i < 3; ++i) t->streams[i] = -1;
    t->resolve = t->reject = t->value = t->callback = JS_UNDEFINED;
    return t;
}

static int enqueue(QJSSystem *s, Task *t)
{
    unsigned i;
    for (i = 0; i < TASKS; ++i) if (!s->tasks[i]) {
        t->sequence = ++s->next_sequence;
        s->tasks[i] = t; return 0;
    }
    JS_ThrowRangeError(s->ctx, "too many pending system operations (limit 32)");
    return -1;
}

static JSValue promise(QJSSystem *s, Task *t)
{
    JSValue functions[2], result = JS_NewPromiseCapability(s->ctx, functions);
    if (!JS_IsException(result)) {
        t->resolve = functions[0]; t->reject = functions[1];
        if (!enqueue(s, t)) return result;
        JS_FreeValue(s->ctx, result);
    }
    task_free(s, t); return JS_EXCEPTION;
}

/* Strings cross the OS boundary as logical UTF-8 octets, never native UTF-16
 * cells. Explicit lengths reject hidden NUL in paths, arguments and env names. */
static char *os_string(JSContext *ctx, JSValueConst value, size_t limit)
{
    const char *text;
    char *copy;
    size_t length;
    if (!JS_IsString(value)) { JS_ThrowTypeError(ctx, "expected a string"); return NULL; }
    text = JS_ToCStringLen(ctx, &length, value);
    if (!text) return NULL;
    if (length > limit || memchr(text, 0, length)) {
        JS_FreeCString(ctx, text);
        JS_ThrowRangeError(ctx, "OS string is too long or contains NUL"); return NULL;
    }
    copy = js_malloc(ctx, length + 1);
    if (copy) memcpy(copy, text, length + 1);
    JS_FreeCString(ctx, text);
    return copy;
}

static int append(JSContext *ctx, Buffer *b, const char *text, size_t n, size_t limit)
{
    size_t capacity;
    char *p;
    if (n > limit - b->size) { errno = EFBIG; return -1; }
    if (b->size + n > b->capacity) {
        capacity = b->capacity ? b->capacity * 2 : CHUNK;
        if (capacity < b->size + n) capacity = b->size + n;
        if (capacity > limit) capacity = limit;
        p = js_realloc(ctx, b->data, capacity);
        if (!p) return -2;
        b->data = p; b->capacity = capacity;
    }
    if (n) memcpy(b->data + b->size, text, n);
    b->size += n;
    return 0;
}

static JSValue file_operation(JSContext *ctx, JSValueConst self, int32_t argc,
                              JSValueConst *argv, int32_t operation)
{
    QJSSystem *s = JS_GetContextOpaque(ctx);
    Task *t = task_new(ctx, (enum Operation)operation);
    const char *text;
    size_t length;
    (void)self;
    if (!t) return JS_EXCEPTION;
    t->path = os_string(ctx, argc ? argv[0] : JS_UNDEFINED, PATH_LIMIT);
    if (!t->path) goto fail;
    if (operation == WRITE_TEXT) {
        if (argc < 2 || !JS_IsString(argv[1])) { JS_ThrowTypeError(ctx, "expected text to write"); goto fail; }
        text = JS_ToCStringLen(ctx, &length, argv[1]);
        if (!text) goto fail;
        if (length > TEXT_LIMIT) {
            JS_FreeCString(ctx, text); JS_ThrowRangeError(ctx, "text exceeds 1 MiB limit"); goto fail;
        }
        if (append(ctx, &t->data, text, length, TEXT_LIMIT)) { JS_FreeCString(ctx, text); goto fail; }
        JS_FreeCString(ctx, text);
    }
    return promise(s, t);
fail:
    task_free(s, t); return JS_EXCEPTION;
}

static JSValue environment(JSContext *ctx, JSValueConst self, int32_t argc, JSValueConst *argv)
{
    char *name = os_string(ctx, argc ? argv[0] : JS_UNDEFINED, PATH_LIMIT);
    const char *value;
    JSValue result;
    (void)self;
    if (!name) return JS_EXCEPTION;
    value = getenv(name);
    result = value ? JS_NewString(ctx, value) : JS_UNDEFINED;
    js_free(ctx, name); return result;
}

static JSValue working_directory(JSContext *ctx, JSValueConst self, int32_t argc, JSValueConst *argv)
{
    char path[PATH_LIMIT + 1];
    JSValue value;
    (void)self; (void)argc; (void)argv;
    if (getcwd(path, sizeof(path))) return JS_NewString(ctx, path);
    value = os_error(ctx, errno, "getcwd", NULL);
    return JS_IsException(value) ? value : JS_Throw(ctx, value);
}

static int delay(JSContext *ctx, JSValueConst arg, uint64_t *due)
{
    double milliseconds;
    uint64_t now;
    if (JS_ToFloat64(ctx, &milliseconds, arg)) return -1;
    if (!(milliseconds >= 0 && milliseconds <= 2147483647.0)) {
        JS_ThrowRangeError(ctx, "delay must be between 0 and 2147483647 ms"); return -1;
    }
    if (qjs_system_now(&now)) {
        JSValue value = os_error(ctx, errno, "clock_gettime", NULL);
        if (!JS_IsException(value)) JS_Throw(ctx, value);
        return -1;
    }
    *due = now + (uint32_t)milliseconds;
    return 0;
}

static JSValue sleep_async(JSContext *ctx, JSValueConst self, int32_t argc, JSValueConst *argv)
{
    QJSSystem *s = JS_GetContextOpaque(ctx);
    Task *t = task_new(ctx, SLEEP);
    (void)self;
    if (!t) return JS_EXCEPTION;
    if (delay(ctx, argc ? argv[0] : JS_UNDEFINED, &t->due)) { task_free(s, t); return JS_EXCEPTION; }
    return promise(s, t);
}

static JSValue set_timeout(JSContext *ctx, JSValueConst self, int32_t argc, JSValueConst *argv)
{
    QJSSystem *s = JS_GetContextOpaque(ctx);
    Task *t;
    int32_t i;
    (void)self;
    if (!argc || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "expected a timer callback");
    if (argc > ARG_LIMIT + 2 || s->next_id == 2147483647UL)
        return JS_ThrowRangeError(ctx, "timer argument or identifier limit exceeded");
    t = task_new(ctx, TIMER);
    if (!t) return JS_EXCEPTION;
    if (delay(ctx, argc > 1 ? argv[1] : JS_NewInt32(ctx, 0), &t->due)) goto fail;
    t->callback = JS_DupValue(ctx, argv[0]);
    if (argc > 2) {
        t->arguments = js_malloc(ctx, (size_t)(argc - 2) * sizeof(JSValue));
        if (!t->arguments) goto fail;
        for (i = 2; i < argc; ++i) t->arguments[t->argc++] = JS_DupValue(ctx, argv[i]);
    }
    t->id = ++s->next_id;
    if (enqueue(s, t)) goto fail;
    return JS_NewUint32(ctx, t->id);
fail:
    task_free(s, t); return JS_EXCEPTION;
}

static JSValue clear_timeout(JSContext *ctx, JSValueConst self, int32_t argc, JSValueConst *argv)
{
    QJSSystem *s = JS_GetContextOpaque(ctx);
    uint32_t id;
    unsigned i;
    (void)self;
    if (JS_ToUint32(ctx, &id, argc ? argv[0] : JS_UNDEFINED)) return JS_EXCEPTION;
    for (i = 0; i < TASKS; ++i) if (s->tasks[i] && s->tasks[i]->op == TIMER && s->tasks[i]->id == id) {
        task_free(s, s->tasks[i]); s->tasks[i] = NULL; break;
    }
    return JS_UNDEFINED;
}

static JSValue run_async(JSContext *ctx, JSValueConst self, int32_t argc, JSValueConst *argv)
{
    QJSSystem *s = JS_GetContextOpaque(ctx);
    Task *t = task_new(ctx, RUN);
    JSValue length = JS_UNDEFINED, item;
    uint32_t count = 0, i;
    uint64_t now;
    size_t total = 0;
    (void)self;
    if (!t) return JS_EXCEPTION;
    t->path = os_string(ctx, argc ? argv[0] : JS_UNDEFINED, PATH_LIMIT);
    if (!t->path) goto fail;
    if (argc > 1) {
        if (JS_IsArray(ctx, argv[1]) != 1) { JS_ThrowTypeError(ctx, "expected an argument array"); goto fail; }
        length = JS_GetPropertyStr(ctx, argv[1], "length");
        if (JS_IsException(length) || JS_ToUint32(ctx, &count, length)) goto fail;
        JS_FreeValue(ctx, length); length = JS_UNDEFINED;
    }
    if (count > ARG_LIMIT) { JS_ThrowRangeError(ctx, "too many program arguments"); goto fail; }
    t->argv = js_mallocz(ctx, (count + 2) * sizeof(char *));
    if (!t->argv) goto fail;
    t->argv[0] = js_strdup(ctx, t->path);
    if (!t->argv[0]) goto fail;
    for (i = 0; i < count; ++i) {
        item = JS_GetPropertyUint32(ctx, argv[1], i);
        if (JS_IsException(item)) goto fail;
        t->argv[i + 1] = os_string(ctx, item, 4096);
        JS_FreeValue(ctx, item);
        if (!t->argv[i + 1]) goto fail;
        total += strlen(t->argv[i + 1]) + 1;
        if (total > 4096) { JS_ThrowRangeError(ctx, "program arguments exceed 4096 octets"); goto fail; }
    }
    if (qjs_system_now(&now)) { fail_loop(s, errno, "clock_gettime"); goto fail; }
    t->due = now + RUN_TIMEOUT;
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        if (!JS_IsObject(argv[2])) { JS_ThrowTypeError(ctx, "expected run options"); goto fail; }
        length = JS_GetPropertyStr(ctx, argv[2], "timeoutMs");
        if (JS_IsException(length)) goto fail;
        if (!JS_IsUndefined(length) && delay(ctx, length, &t->due)) goto fail;
        JS_FreeValue(ctx, length); length = JS_UNDEFINED;
        length = JS_GetPropertyStr(ctx, argv[2], "maxOutputBytes");
        if (JS_IsException(length)) goto fail;
        if (!JS_IsUndefined(length)) {
            double limit;
            if (JS_ToFloat64(ctx, &limit, length)) goto fail;
            if (!(limit >= 1 && limit <= TEXT_LIMIT) || limit != (uint32_t)limit) {
                JS_ThrowRangeError(ctx, "maxOutputBytes must be an integer in 1..1048576"); goto fail;
            }
            t->output_limit = (size_t)limit;
        }
        JS_FreeValue(ctx, length); length = JS_GetPropertyStr(ctx, argv[2], "binary");
        if (JS_IsException(length)) goto fail;
        t->binary = JS_ToBool(ctx, length);
        if (t->binary < 0) goto fail;
        JS_FreeValue(ctx, length); length = JS_UNDEFINED;
        t->value = JS_GetPropertyStr(ctx, argv[2], "signal");
        if (JS_IsException(t->value)) { t->value = JS_UNDEFINED; goto fail; }
        if (!JS_IsUndefined(t->value) && !JS_IsObject(t->value)) {
            JS_ThrowTypeError(ctx, "expected a signal object"); goto fail;
        }
    }
    return promise(s, t);
fail:
    JS_FreeValue(ctx, length); task_free(s, t); return JS_EXCEPTION;
}

/* These octet helpers preserve binary worker output on both 8-bit hosts and
 * C16 guests. UTF-8 decoding uses the engine's replacement policy. */
static JSValue decode_utf8(JSContext *ctx, JSValueConst self, int32_t argc, JSValueConst *argv)
{
    size_t length;
    uint8_t *bytes;
    (void)self;
    bytes = JS_GetArrayBuffer(ctx, &length, argc ? argv[0] : JS_UNDEFINED);
    if (!bytes) return JS_EXCEPTION;
    return JS_NewStringLen(ctx, (const char *)bytes, length);
}

static JSValue encode_utf8(JSContext *ctx, JSValueConst self, int32_t argc, JSValueConst *argv)
{
    const char *text; size_t length; JSValue result;
    (void)self;
    if (!argc || !JS_IsString(argv[0])) return JS_ThrowTypeError(ctx, "expected a string");
    text = JS_ToCStringLen(ctx, &length, argv[0]);
    if (!text) return JS_EXCEPTION;
    result = length > TEXT_LIMIT ? JS_ThrowRangeError(ctx, "UTF-8 text exceeds 1 MiB") :
        JS_NewArrayBufferCopy(ctx, (const uint8_t *)text, length);
    JS_FreeCString(ctx, text); return result;
}

static JSValue monotonic_now(JSContext *ctx, JSValueConst self, int32_t argc, JSValueConst *argv)
{
    uint64_t now;
    (void)self; (void)argc; (void)argv;
    if (qjs_system_now(&now)) { fail_loop(JS_GetContextOpaque(ctx), errno, "clock_gettime"); return JS_EXCEPTION; }
    return JS_NewFloat64(ctx, (double)now);
}

JSModuleDef *qjs_system_module(JSContext *ctx, const char *name)
{
    JSValue compiled;
    JSModuleDef *module;
    const char *source; size_t length;
    if (!strcmp(name, "fuse:http")) { source = qjs_http_source; length = sizeof(qjs_http_source) - 1; }
    else if (!strcmp(name, "fuse:net")) { source = qjs_net_source; length = sizeof(qjs_net_source) - 1; }
    else if (!strcmp(name, "fuse:http-server")) { source = qjs_server_source; length = sizeof(qjs_server_source) - 1; }
    else return NULL;
    compiled = JS_Eval(ctx, source, length, name,
                       JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(compiled)) return NULL;
    module = JS_VALUE_GET_PTR(compiled);
    JS_FreeValue(ctx, compiled);
    return module;
}

static int exported(JSContext *ctx, JSModuleDef *m, const char *name, JSCFunction *function, int32_t argc)
{
    JSValue value = JS_NewCFunction(ctx, function, name, argc);
    return JS_IsException(value) ? -1 : JS_SetModuleExport(ctx, m, name, value);
}

static int32_t module_init(JSContext *ctx, JSModuleDef *m)
{
    static const char *names[] = {"readTextFile", "readDir", "stat", "writeTextFile"};
    int32_t i;
    JSValue value;
    for (i = 0; i < 4; ++i) {
        value = JS_NewCFunctionMagic(ctx, file_operation, names[i], i == WRITE_TEXT ? 2 : 1, JS_CFUNC_generic_magic, i);
        if (JS_IsException(value) || JS_SetModuleExport(ctx, m, names[i], value) < 0) return -1;
    }
    if (exported(ctx, m, "getenv", environment, 1) < 0 ||
        exported(ctx, m, "cwd", working_directory, 0) < 0 ||
        exported(ctx, m, "run", run_async, 2) < 0 ||
        exported(ctx, m, "sleep", sleep_async, 1) < 0 ||
        exported(ctx, m, "decodeUTF8", decode_utf8, 1) < 0 ||
        exported(ctx, m, "encodeUTF8", encode_utf8, 1) < 0 ||
        exported(ctx, m, "monotonicNow", monotonic_now, 0) < 0) return -1;
    return 0;
}

static int global_function(JSContext *ctx, JSValueConst global, const char *name, JSCFunction *function, int32_t argc)
{
    JSValue value = JS_NewCFunction(ctx, function, name, argc);
    return JS_IsException(value) ? -1 : property(ctx, global, name, value);
}

static int socket_register(JSContext *ctx);

QJSSystem *qjs_system_new(JSContext *ctx)
{
    static const char *names[] = {"readTextFile", "readDir", "stat", "writeTextFile", "getenv", "cwd", "run", "sleep", "decodeUTF8", "monotonicNow", "encodeUTF8"};
    QJSSystem *s;
    JSModuleDef *module;
    JSValue global;
    int i, failed;
    if (JS_GetContextOpaque(ctx)) { JS_ThrowInternalError(ctx, "context opaque pointer is already in use"); return NULL; }
    s = js_mallocz(ctx, sizeof(*s));
    if (!s) return NULL;
    s->ctx = ctx; JS_SetContextOpaque(ctx, s);
    module = JS_NewCModule(ctx, "fuse:system", module_init);
    if (!module) goto fail;
    for (i = 0; i < 11; ++i) if (JS_AddModuleExport(ctx, module, names[i]) < 0) goto fail;
    if (socket_register(ctx)) goto fail;
    global = JS_GetGlobalObject(ctx);
    failed = global_function(ctx, global, "setTimeout", set_timeout, 2) < 0 ||
             global_function(ctx, global, "clearTimeout", clear_timeout, 1) < 0;
    JS_FreeValue(ctx, global);
    if (failed) goto fail;
    return s;
fail:
    qjs_system_free(s); return NULL;
}

static int finish(QJSSystem *s, unsigned index, JSValue value, int reject)
{
    Task *t = s->tasks[index];
    JSValue result;
    s->tasks[index] = NULL; /* callbacks may cancel or enqueue other tasks */
    if (JS_IsException(value)) { task_free(s, t); return -1; }
    result = JS_Call(s->ctx, reject ? t->reject : t->resolve, JS_UNDEFINED, 1, &value);
    JS_FreeValue(s->ctx, value); task_free(s, t);
    if (JS_IsException(result)) return -1;
    JS_FreeValue(s->ctx, result); return 1;
}

static int reject_task(QJSSystem *s, unsigned index, int error, const char *operation)
{
    JSValue value = os_error(s->ctx, error, operation, s->tasks[index]->path);
    if (s->tasks[index]->cancelled && !JS_IsException(value) &&
        property(s->ctx, value, "name", JS_NewString(s->ctx, "AbortError")) < 0) {
        JS_FreeValue(s->ctx, value); value = JS_EXCEPTION;
    }
    return finish(s, index, value, 1);
}

static int descriptor(int fd)
{
    if (fd >= FD_SETSIZE) { errno = EMFILE; return -1; }
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || fcntl(fd, F_SETFL, O_NONBLOCK) < 0) return -1;
    return 0;
}

#include "sockets.inc"

static int spawn(Task *t)
{
    int pipes[3][2], i, j, nullfd = -1, error = 0;
    pid_t pid;
    for (i = 0; i < 3; ++i) pipes[i][0] = pipes[i][1] = -1;
    for (i = 0; i < 3; ++i) {
        if (pipe(pipes[i]) < 0) goto fail;
        if (pipes[i][0] < 3 || pipes[i][1] < 3) { errno = EBADF; goto fail; }
        if (descriptor(pipes[i][0]) < 0 || fcntl(pipes[i][1], F_SETFD, FD_CLOEXEC) < 0) goto fail;
    }
    nullfd = open("/dev/null", O_RDONLY);
    if (nullfd < 0) goto fail;
    pid = fork();
    if (pid < 0) goto fail;
    if (!pid) {
        if (setpgid(0, 0) < 0 || dup2(nullfd, 0) < 0 ||
            dup2(pipes[0][1], 1) < 0 || dup2(pipes[1][1], 2) < 0) goto child_error;
        /* The guest's dup2 currently copies CLOEXEC. Set the intended flags
         * explicitly, also making this host work with existing disk kernels. */
        if (fcntl(0, F_SETFD, 0) < 0 || fcntl(1, F_SETFD, 0) < 0 ||
            fcntl(2, F_SETFD, 0) < 0) goto child_error;
        /* Close all inherited task descriptors, including other children.
         * No JS/libc allocation or output flushing occurs after fork. */
        for (i = 3; i < FD_SETSIZE; ++i) if (i != pipes[2][1]) close(i);
        execvp(t->path, t->argv);
child_error:
        error = errno;
        while (write(pipes[2][1], &error, sizeof(error)) < 0 && errno == EINTR) {}
        _exit(127);
    }
    t->pid = pid;
    close(nullfd);
    for (i = 0; i < 3; ++i) { close(pipes[i][1]); t->streams[i] = pipes[i][0]; }
    return 0;
fail:
    error = errno;
    if (nullfd >= 0) close(nullfd);
    for (i = 0; i < 3; ++i) for (j = 0; j < 2; ++j) if (pipes[i][j] >= 0) close(pipes[i][j]);
    errno = error; return -1;
}

static int run_step(QJSSystem *s, unsigned index, fd_set *ready, uint64_t now)
{
    Task *t = s->tasks[index];
    char buffer[CHUNK];
    ssize_t n;
    int i, rc, error;
    pid_t reaped;
    JSValue result;
    if (!JS_IsUndefined(t->value) && !t->failure) {
        JSValue aborted = JS_GetPropertyStr(s->ctx, t->value, "aborted");
        int cancelled;
        if (JS_IsException(aborted)) return -1;
        cancelled = JS_ToBool(s->ctx, aborted); JS_FreeValue(s->ctx, aborted);
        if (cancelled < 0) return -1;
        if (cancelled) { t->cancelled = 1; t->failure = EINTR; }
    }
    if (!t->pid && t->failure) return reject_task(s, index, t->failure, "run");
    if (!t->pid) {
        if (now >= t->due) return reject_task(s, index, ETIMEDOUT, "run");
        if (spawn(t)) return reject_task(s, index, errno, "spawn");
        return 1;
    }
    if (now >= t->due && !t->failure) t->failure = ETIMEDOUT;
    if (t->failure) {
        kill_child(t);
        for (i = 0; i < 3; ++i) if (t->streams[i] >= 0) { close(t->streams[i]); t->streams[i] = -1; }
    } else for (i = 0; i < 3; ++i) if (t->streams[i] >= 0 && FD_ISSET(t->streams[i], ready)) {
        n = read(t->streams[i], buffer, sizeof(buffer));
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n < 0) { t->failure = errno; break; }
        if (!n) { close(t->streams[i]); t->streams[i] = -1; continue; }
#if CHAR_BIT > 8
        if (i != 2) {
            size_t at;
            for (at = 0; at < (size_t)n; ++at) if ((unsigned char)buffer[at] > 255U) break;
            if (at != (size_t)n) { t->failure = EINVAL; break; }
        }
#endif
        if (i == 2) {
            if ((size_t)n > sizeof(t->exec_error) - t->exec_size) { t->failure = EIO; break; }
            memcpy(t->exec_error + t->exec_size, buffer, n); t->exec_size += n;
        } else {
            rc = append(s->ctx, i ? &t->error : &t->output, buffer, n, i ? OUTPUT_LIMIT : t->output_limit);
            if (rc == -2) return -1;
            if (rc) { t->failure = EFBIG; break; }
        }
    }
    if (!t->reaped) {
        reaped = waitpid(t->pid, &t->status, WNOHANG);
        if (reaped < 0 && errno != EINTR) {
            error = errno;
            if (error == ECHILD) { t->reaped = 1; t->pid = 0; }
            return reject_task(s, index, error, "waitpid");
        }
        if (reaped == t->pid) t->reaped = 1;
    }
    if (t->failure) kill_child(t);
    if (t->reaped && t->failure) return reject_task(s, index, t->failure, "run");
    for (i = 0; i < 3; ++i) if (t->streams[i] >= 0) return 1;
    if (!t->reaped) return 1;
    if (t->exec_size) {
        error = EIO;
        if (t->exec_size == sizeof(error)) memcpy(&error, t->exec_error, sizeof(error));
        return reject_task(s, index, error, "execvp");
    }
    result = JS_NewObject(s->ctx);
    if (JS_IsException(result)) return -1;
    if (property(s->ctx, result, "code", WIFEXITED(t->status) ? JS_NewInt32(s->ctx, WEXITSTATUS(t->status)) : JS_NULL) < 0 ||
        property(s->ctx, result, "signal", WIFSIGNALED(t->status) ? JS_NewInt32(s->ctx, WTERMSIG(t->status)) : JS_NULL) < 0 ||
        property(s->ctx, result, "stdout", t->binary ? JS_NewArrayBufferCopy(s->ctx, (const uint8_t *)(t->output.data ? t->output.data : ""), t->output.size) : JS_NewStringLen(s->ctx, t->output.data ? t->output.data : "", t->output.size)) < 0 ||
        property(s->ctx, result, "stderr", JS_NewStringLen(s->ctx, t->error.data ? t->error.data : "", t->error.size)) < 0) {
        JS_FreeValue(s->ctx, result); return -1;
    }
    return finish(s, index, result, 0);
}

static int file_step(QJSSystem *s, unsigned index, fd_set *ready)
{
    Task *t = s->tasks[index];
    JSContext *ctx = s->ctx;
    struct stat info;
    struct dirent *entry;
    JSValue result, name;
    char buffer[CHUNK];
    ssize_t n;
    int rc, error, i;
    size_t amount, j;
    if (t->op == STAT_PATH) {
        if (stat(t->path, &info)) return reject_task(s, index, errno, "stat");
        result = JS_NewObject(ctx);
        if (JS_IsException(result)) return -1;
        if (property(ctx, result, "size", JS_NewFloat64(ctx, (double)info.st_size)) < 0 ||
            property(ctx, result, "mode", JS_NewUint32(ctx, info.st_mode)) < 0 ||
            property(ctx, result, "isDirectory", JS_NewBool(ctx, S_ISDIR(info.st_mode))) < 0 ||
            property(ctx, result, "isFile", JS_NewBool(ctx, S_ISREG(info.st_mode))) < 0) {
            JS_FreeValue(ctx, result); return -1;
        }
        return finish(s, index, result, 0);
    }
    if (t->op == READ_DIR) {
        if (!t->directory) {
            t->directory = opendir(t->path);
            if (!t->directory) return reject_task(s, index, errno, "opendir");
            t->value = JS_NewArray(ctx);
            if (JS_IsException(t->value)) return -1;
        }
        for (i = 0; i < 16; ++i) {
            errno = 0; entry = readdir(t->directory);
            if (!entry) {
                error = errno;
                rc = closedir(t->directory); t->directory = NULL;
                if (error || rc < 0) return reject_task(s, index, error ? error : errno, "readdir");
                return finish(s, index, JS_DupValue(ctx, t->value), 0);
            }
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            if (t->entries == ENTRIES_LIMIT) return reject_task(s, index, EFBIG, "readdir");
            name = JS_NewString(ctx, entry->d_name);
            if (JS_IsException(name) || JS_SetPropertyUint32(ctx, t->value, t->entries++, name) < 0) return -1;
        }
        return 1;
    }
    if (t->fd < 0) {
        /* Nonblocking open plus fstat excludes devices/FIFOs from the text
         * convenience API. Pipe readiness belongs to run(), below. */
        t->fd = open(t->path, (t->op == WRITE_TEXT ? O_WRONLY | O_CREAT | O_TRUNC : O_RDONLY) | O_NONBLOCK, 0666);
        if (t->fd < 0) return reject_task(s, index, errno, "open");
        if (descriptor(t->fd) < 0) return reject_task(s, index, errno, "fcntl");
        if (fstat(t->fd, &info)) return reject_task(s, index, errno, "fstat");
        if (!S_ISREG(info.st_mode)) return reject_task(s, index, S_ISDIR(info.st_mode) ? EISDIR : EINVAL, "text file");
        if (t->op == READ_TEXT && (info.st_size < 0 || (uint64_t)info.st_size > TEXT_LIMIT))
            return reject_task(s, index, EFBIG, "read");
        return 1;
    }
    if (!FD_ISSET(t->fd, ready)) return 0;
    if (t->op == WRITE_TEXT) {
        amount = t->data.size - t->entries;
        if (amount > CHUNK) amount = CHUNK;
        n = amount ? write(t->fd, t->data.data + t->entries, amount) : 0;
        if (n > 0) t->entries += n;
        if (t->entries == t->data.size) {
            rc = close(t->fd); t->fd = -1;
            if (rc < 0) return reject_task(s, index, errno, "close");
            return finish(s, index, JS_UNDEFINED, 0);
        }
    } else {
        n = read(t->fd, buffer, sizeof(buffer));
        if (n > 0) {
            for (j = 0; j < (size_t)n; ++j) if ((unsigned char)buffer[j] > 255U) return reject_task(s, index, EINVAL, "read octets");
            rc = append(ctx, &t->data, buffer, n, TEXT_LIMIT);
            if (rc == -2) return -1;
            if (rc) return reject_task(s, index, EFBIG, "read");
        } else if (!n) return finish(s, index, JS_NewStringLen(ctx, t->data.data ? t->data.data : "", t->data.size), 0);
    }
    if (n < 0 && errno != EINTR && errno != EAGAIN) return reject_task(s, index, errno, t->op == WRITE_TEXT ? "write" : "read");
    if (!n && t->op == WRITE_TEXT) return reject_task(s, index, EIO, "write");
    return 1;
}

int qjs_system_step(QJSSystem *s, unsigned long max_wait_ms)
{
    fd_set reads, writes, ready;
    struct timeval timeout;
    Task *t;
    uint64_t now, wait = max_wait_ms < 1000 ? max_wait_ms : 1000;
    int maxfd = -1, immediate = 0, result, i, j, is_ready, first_timer = -1;
    unsigned index;
    JSValue value;
    if (!qjs_system_pending(s)) return 0;
    if (qjs_system_now(&now)) return fail_loop(s, errno, "clock_gettime");
    FD_ZERO(&reads); FD_ZERO(&writes);
    for (i = 0; i < TASKS; ++i) if ((t = s->tasks[i])) {
        if (t->op == TIMER || t->op == SLEEP || t->op == RUN || t->op >= NET_ACCEPT) {
            if (t->due <= now) immediate = 1;
            else if (t->due - now < wait) wait = t->due - now;
        }
        if (t->op >= NET_ACCEPT) {
            Socket *socket = socket_find(s, t->socket_id);
            if (!socket || t->failure || (t->op == NET_CONNECT && !t->entries) ||
                (t->op == NET_WRITE && t->entries == t->data.size)) immediate = 1;
            else {
                if (t->op == NET_WRITE || t->op == NET_CONNECT) FD_SET(socket->fd, &writes);
                else FD_SET(socket->fd, &reads);
                if (socket->fd > maxfd) maxfd = socket->fd;
            }
            if (!JS_IsUndefined(t->value) && wait > 10) wait = 10;
        } else if (t->op == RUN) {
            if (!t->pid || t->failure) immediate = 1;
            if (wait > 10) wait = 10; /* WNOHANG closes the exit/pipe EOF race. */
            for (j = 0; j < 3; ++j) if (t->streams[j] >= 0) {
                FD_SET(t->streams[j], &reads);
                if (t->streams[j] > maxfd) maxfd = t->streams[j];
            }
        } else if (t->op != TIMER && t->op != SLEEP) {
            if (t->fd < 0) immediate = 1;
            else {
                if (t->op == WRITE_TEXT) FD_SET(t->fd, &writes); else FD_SET(t->fd, &reads);
                if (t->fd > maxfd) maxfd = t->fd;
            }
        }
    }
    if (immediate) wait = 0;
    timeout.tv_sec = wait / 1000; timeout.tv_usec = (wait % 1000) * 1000;
    result = select(maxfd + 1, &reads, &writes, NULL, &timeout);
    if (result < 0) return errno == EINTR ? 0 : fail_loop(s, errno, "select");
    if (qjs_system_now(&now)) return fail_loop(s, errno, "clock_gettime");
    /* Round-robin slot reuse must not reorder timers that became overdue
     * during JS execution or a syscall. Choose deadline order, then enqueue
     * order, within the timed work; ordinary I/O still gets its fair turn. */
    for (i = 0; i < TASKS; ++i) if ((t = s->tasks[i]) &&
        (t->op == TIMER || t->op == SLEEP) && t->due <= now) {
        if (first_timer < 0 || t->due < s->tasks[first_timer]->due ||
            (t->due == s->tasks[first_timer]->due && t->sequence < s->tasks[first_timer]->sequence))
            first_timer = i;
    }
    for (i = 0; i < TASKS; ++i) {
        index = (s->cursor + i) % TASKS;
        t = s->tasks[index];
        if (!t) continue;
        if ((t->op == TIMER || t->op == SLEEP) && (int)index != first_timer) continue;
        if (t->op >= NET_ACCEPT) {
            Socket *socket = socket_find(s, t->socket_id);
            is_ready = !socket || now >= t->due || !JS_IsUndefined(t->value) ||
                (t->op == NET_CONNECT && !t->entries) ||
                (t->op == NET_WRITE && t->entries == t->data.size) ||
                FD_ISSET(socket->fd, t->op == NET_WRITE || t->op == NET_CONNECT ? &writes : &reads);
        } else is_ready = t->op == RUN || ((t->op == TIMER || t->op == SLEEP) ? now >= t->due :
                   (t->fd < 0 || FD_ISSET(t->fd, t->op == WRITE_TEXT ? &writes : &reads)));
        if (!is_ready) continue;
        s->cursor = (index + 1) % TASKS;
        if (t->op == SLEEP) return finish(s, index, JS_UNDEFINED, 0);
        if (t->op == TIMER) {
            s->tasks[index] = NULL;
            value = JS_Call(s->ctx, t->callback, JS_UNDEFINED, t->argc, t->arguments);
            task_free(s, t);
            if (JS_IsException(value)) return -1;
            JS_FreeValue(s->ctx, value); return 1;
        }
        if (t->op >= NET_ACCEPT) return socket_step(s, index, &reads, &writes, now);
        if (t->op == RUN) return run_step(s, index, &reads, now);
        ready = t->op == WRITE_TEXT ? writes : reads;
        return file_step(s, index, &ready);
    }
    return 0;
}
