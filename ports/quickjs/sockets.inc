/* SPDX-License-Identifier: MIT */
/* Context-owned handles, never raw JS file descriptors. IDs are not reused,
 * so queued work cannot act on a descriptor reused after close(). */
static int number_in(JSContext *ctx, JSValueConst value, uint32_t lo, uint32_t hi, uint32_t *out)
{
    double n;
    if (JS_ToFloat64(ctx, &n, value)) return -1;
    if (!(n >= lo && n <= hi) || n != (uint32_t)n) {
        JS_ThrowRangeError(ctx, "socket argument outside bounds"); return -1;
    }
    *out = (uint32_t)n; return 0;
}
static int ipv4(JSContext *ctx, const char *text, unsigned port, struct sockaddr_in *address)
{
    uint32_t ip = 0; unsigned i, n, digits;
    memset(address, 0, sizeof(*address)); address->sin_family = AF_INET; address->sin_port = htons(port);
    for (i = 0; i < 4; ++i) {
        n = digits = 0;
        while (*text >= '0' && *text <= '9') {
            if (++digits > 3) goto invalid;
            n = n * 10 + *text++ - '0';
        }
        if (!digits || n > 255 || (i < 3 ? *text++ != '.' : *text != 0)) goto invalid;
        ip = (ip << 8) | n;
    }
    address->sin_addr.s_addr = htonl(ip); return 0;
invalid:
    JS_ThrowTypeError(ctx, "expected a numeric IPv4 address"); return -1;
}
static Socket *socket_adopt(QJSSystem *s, int fd, int listener)
{
    unsigned i;
    if (descriptor(fd)) { int error = errno; close(fd); fail_loop(s, error, "socket flags"); return NULL; }
#ifdef SO_NOSIGPIPE
    { int yes = 1; if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes))) {
        int error = errno; close(fd); fail_loop(s, error, "SO_NOSIGPIPE"); return NULL;
    } }
#endif
    for (i = 0; i < TASKS; ++i) if (!s->sockets[i].id) break;
    if (i == TASKS || s->next_socket == 2147483647UL) {
        close(fd); JS_ThrowRangeError(s->ctx, "socket handle limit exceeded (32)"); return NULL;
    }
    s->sockets[i].id = ++s->next_socket;
    s->sockets[i].fd = fd; s->sockets[i].listener = listener;
    return &s->sockets[i];
}
static JSValue socket_listen(JSContext *ctx, JSValueConst self, int32_t argc, JSValueConst *argv)
{
    QJSSystem *s = JS_GetContextOpaque(ctx); Socket *owned;
    uint32_t port; char *host; struct sockaddr_in address; int fd, error;
    (void)self;
    if (argc < 2) return JS_ThrowTypeError(ctx, "expected hostname and port");
    if (number_in(ctx, argv[1], 0, 65535, &port)) return JS_EXCEPTION;
    host = os_string(ctx, argv[0], 15); if (!host) return JS_EXCEPTION;
    error = ipv4(ctx, host, port, &address); js_free(ctx, host); if (error) return JS_EXCEPTION;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { fail_loop(s, errno, "socket"); return JS_EXCEPTION; }
#ifndef RIPPLE_MMU
    { int yes = 1; if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes))) goto fail; }
#endif
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) || listen(fd, 4)) goto fail;
    owned = socket_adopt(s, fd, 1);
    return owned ? JS_NewUint32(ctx, owned->id) : JS_EXCEPTION;
fail:
    error = errno; close(fd); fail_loop(s, error, "listen"); return JS_EXCEPTION;
}
static JSValue socket_close(JSContext *ctx, JSValueConst self, int32_t argc, JSValueConst *argv)
{
    uint32_t id; (void)self;
    if (number_in(ctx, argc ? argv[0] : JS_UNDEFINED, 1, 2147483647UL, &id)) return JS_EXCEPTION;
    socket_drop(socket_find(JS_GetContextOpaque(ctx), id)); return JS_UNDEFINED;
}
static JSValue socket_address(JSContext *ctx, JSValueConst self, int32_t argc, JSValueConst *argv)
{
    QJSSystem *s = JS_GetContextOpaque(ctx); Socket *socket;
    uint32_t id, ip; char text[16]; struct sockaddr_in address; socklen_t length = sizeof(address);
    int peer, rc; JSValue result; (void)self;
    if (number_in(ctx, argc ? argv[0] : JS_UNDEFINED, 1, 2147483647UL, &id)) return JS_EXCEPTION;
    peer = argc > 1 ? JS_ToBool(ctx, argv[1]) : 0; if (peer < 0) return JS_EXCEPTION;
    socket = socket_find(s, id); if (!socket) { fail_loop(s, EBADF, "socket address"); return JS_EXCEPTION; }
    rc = peer ? getpeername(socket->fd, (struct sockaddr *)&address, &length) :
                getsockname(socket->fd, (struct sockaddr *)&address, &length);
    if (rc) { fail_loop(s, errno, "socket address"); return JS_EXCEPTION; }
    ip = ntohl(address.sin_addr.s_addr);
    snprintf(text, sizeof(text), "%lu.%lu.%lu.%lu", (unsigned long)(ip >> 24),
        (unsigned long)((ip >> 16) & 255), (unsigned long)((ip >> 8) & 255), (unsigned long)(ip & 255));
    result = JS_NewObject(ctx); if (JS_IsException(result)) return result;
    if (property(ctx, result, "hostname", JS_NewString(ctx, text)) < 0 ||
        property(ctx, result, "port", JS_NewUint32(ctx, ntohs(address.sin_port))) < 0) {
        JS_FreeValue(ctx, result); return JS_EXCEPTION;
    }
    return result;
}
static JSValue socket_operation(JSContext *ctx, JSValueConst self, int32_t argc,
                                JSValueConst *argv, int32_t operation)
{
    QJSSystem *s = JS_GetContextOpaque(ctx); Task *t = task_new(ctx, (enum Operation)operation);
    Socket *owned; uint32_t id = 0, count; JSValue option = JS_UNDEFINED;
    uint8_t *bytes; size_t length; uint64_t now; struct sockaddr_in address;
    int fd, option_index = operation == NET_ACCEPT ? 1 : 2; (void)self;
    if (!t) return JS_EXCEPTION;
    if (qjs_system_now(&now)) { fail_loop(s, errno, "clock_gettime"); goto fail; }
    t->due = now + RUN_TIMEOUT;
    if (argc > option_index && !JS_IsUndefined(argv[option_index])) {
        if (!JS_IsObject(argv[option_index])) { JS_ThrowTypeError(ctx, "expected socket options"); goto fail; }
        option = JS_GetPropertyStr(ctx, argv[option_index], "timeoutMs");
        if (JS_IsException(option) || (!JS_IsUndefined(option) && delay(ctx, option, &t->due))) goto fail;
        JS_FreeValue(ctx, option); option = JS_UNDEFINED;
        t->value = JS_GetPropertyStr(ctx, argv[option_index], "signal");
        if (JS_IsException(t->value)) { t->value = JS_UNDEFINED; goto fail; }
        if (!JS_IsUndefined(t->value) && !JS_IsObject(t->value)) { JS_ThrowTypeError(ctx, "expected signal object"); goto fail; }
    }
    if (operation == NET_CONNECT) {
        if (argc < 2) { JS_ThrowTypeError(ctx, "expected hostname and port"); goto fail; }
        if (number_in(ctx, argv[1], 1, 65535, &count)) goto fail;
        t->path = os_string(ctx, argv[0], 15); if (!t->path || ipv4(ctx, t->path, count, &address)) goto fail;
        t->output_limit = count;
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { fail_loop(s, errno, "socket"); goto fail; }
        owned = socket_adopt(s, fd, 0); if (!owned) goto fail;
        t->socket_id = owned->id; owned->writing = 1;
        return promise(s, t);
    }
    if (number_in(ctx, argc ? argv[0] : JS_UNDEFINED, 1, 2147483647UL, &id)) goto fail;
    if (operation == NET_READ) {
        if (number_in(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, 1, 16384, &count)) goto fail;
        t->output_limit = count;
    } else if (operation == NET_WRITE) {
        if (argc < 2) { JS_ThrowTypeError(ctx, "expected ArrayBuffer"); goto fail; }
        bytes = JS_GetArrayBuffer(ctx, &length, argv[1]); if (!bytes) goto fail;
        if (length > 65536UL) { JS_ThrowRangeError(ctx, "socket writes are limited to 64 KiB"); goto fail; }
        if (append(ctx, &t->data, (const char *)bytes, length, 65536UL)) goto fail;
    }
    owned = socket_find(s, id);
    if (!owned) { fail_loop(s, EBADF, "socket"); goto fail; }
    if ((operation == NET_ACCEPT) != !!owned->listener) { JS_ThrowTypeError(ctx, "wrong socket operation"); goto fail; }
    if (operation == NET_WRITE ? owned->writing : owned->reading) { fail_loop(s, EBUSY, "socket operation"); goto fail; }
    t->socket_id = id;
    if (operation == NET_WRITE) owned->writing = 1; else owned->reading = 1;
    return promise(s, t);
fail:
    JS_FreeValue(ctx, option); task_free(s, t); return JS_EXCEPTION;
}
static int32_t socket_module_init(JSContext *ctx, JSModuleDef *module)
{
    static const char *names[] = {"accept", "read", "write", "connect"};
    int32_t i; JSValue value;
    for (i = 0; i < 4; ++i) {
        value = JS_NewCFunctionMagic(ctx, socket_operation, names[i], 3, JS_CFUNC_generic_magic, NET_ACCEPT + i);
        if (JS_IsException(value) || JS_SetModuleExport(ctx, module, names[i], value) < 0) return -1;
    }
    return exported(ctx, module, "listen", socket_listen, 2) < 0 ||
           exported(ctx, module, "close", socket_close, 1) < 0 ||
           exported(ctx, module, "address", socket_address, 2) < 0 ? -1 : 0;
}
static int socket_register(JSContext *ctx)
{
    static const char *names[] = {"accept", "read", "write", "connect", "listen", "close", "address"};
    JSModuleDef *module = JS_NewCModule(ctx, "fuse:net-native", socket_module_init);
    unsigned i; if (!module) return -1;
    for (i = 0; i < 7; ++i) if (JS_AddModuleExport(ctx, module, names[i]) < 0) return -1;
    return 0;
}
static int socket_step(QJSSystem *s, unsigned index, fd_set *reads, fd_set *writes, uint64_t now)
{
    Task *t = s->tasks[index]; Socket *socket, *child; JSContext *ctx = s->ctx;
    JSValue aborted; int cancelled, error = 0, fd, rc; ssize_t n; size_t amount;
    socklen_t length; struct sockaddr_in address; char data[CHUNK];
    if (!JS_IsUndefined(t->value)) {
        aborted = JS_GetPropertyStr(ctx, t->value, "aborted"); if (JS_IsException(aborted)) return -1;
        cancelled = JS_ToBool(ctx, aborted); JS_FreeValue(ctx, aborted); if (cancelled < 0) return -1;
        if (cancelled) { t->cancelled = 1; error = EINTR; }
    }
    socket = socket_find(s, t->socket_id);
    if (!socket) return reject_task(s, index, EBADF, "socket closed");
    if (!error && now >= t->due) error = ETIMEDOUT;
    if (error) {
        if (t->op != NET_ACCEPT) socket_drop(socket);
        return reject_task(s, index, error, "socket");
    }
    if (t->op == NET_CONNECT) {
        if (!t->entries) {
            if (ipv4(ctx, t->path, (unsigned)t->output_limit, &address)) return -1;
            rc = connect(socket->fd, (struct sockaddr *)&address, sizeof(address));
            if (!rc) { t->entries = 2; return finish(s, index, JS_NewUint32(ctx, socket->id), 0); }
            if (errno != EINPROGRESS && errno != EALREADY && errno != EAGAIN && errno != EINTR) goto failure;
            t->entries = 1; return 1;
        }
        if (!FD_ISSET(socket->fd, writes)) return 0;
        length = sizeof(error);
        if (getsockopt(socket->fd, SOL_SOCKET, SO_ERROR, &error, &length)) goto failure;
        if (error) { errno = error; goto failure; }
        length = sizeof(address);
        if (getpeername(socket->fd, (struct sockaddr *)&address, &length)) goto failure;
        t->entries = 2; return finish(s, index, JS_NewUint32(ctx, socket->id), 0);
    }
    if (t->op == NET_WRITE) {
        if (t->entries == t->data.size) return finish(s, index, JS_UNDEFINED, 0);
        if (!FD_ISSET(socket->fd, writes)) return 0;
        amount = t->data.size - t->entries; if (amount > CHUNK) amount = CHUNK;
#ifdef MSG_NOSIGNAL
        n = send(socket->fd, t->data.data + t->entries, amount, MSG_NOSIGNAL);
#else
        n = send(socket->fd, t->data.data + t->entries, amount, 0);
#endif
        if (n > 0) { t->entries += n; return t->entries == t->data.size ? finish(s, index, JS_UNDEFINED, 0) : 1; }
        if (!n) { errno = EPIPE; goto failure; }
    } else {
        if (!FD_ISSET(socket->fd, reads)) return 0;
        if (t->op == NET_ACCEPT) {
            fd = accept(socket->fd, NULL, NULL);
            if (fd >= 0) {
                child = socket_adopt(s, fd, 0);
                if (!child) return -1;
                return finish(s, index, JS_NewUint32(ctx, child->id), 0);
            }
        } else {
            amount = t->output_limit; if (amount > CHUNK) amount = CHUNK;
            n = recv(socket->fd, data, amount, 0);
            if (!n) return finish(s, index, JS_NULL, 0);
            if (n > 0) return finish(s, index, JS_NewArrayBufferCopy(ctx, (const uint8_t *)data, n), 0);
        }
    }
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return 0;
failure:
    error = errno; if (t->op != NET_ACCEPT) socket_drop(socket);
    return reject_task(s, index, error, "socket");
}
