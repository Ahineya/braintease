#include "registry.h"
static void rfl_server_drop(RflNet *n, unsigned short final) {
    unsigned short i;
    if (!n->server) return;
    net_tcp_unlisten(&n->server->listener); n->server->accepting = 0;
    for (i = 0; i < NET_TCP_CONNECTIONS; ++i) {
        if (n->server->pool[i].active) net_tcp_abort(n->server->pool + i);
        if (final) free(n->server->bodies[i]);
    }
    if (final) { free(n->server); n->server = 0; }
}
static unsigned long sequence(void *user, const unsigned char *peer, unsigned short port, unsigned long now) {
    RflNet *n = user;
    /* Application supplies the seed. This mixing is not a host entropy source. */
    n->serial += 0x9e3779b9UL;
    return n->serial ^ now ^ ((unsigned long)port << 16);
}
void rfl_server_clear(RflNet *n, unsigned short i) {
    free(n->server->bodies[i]); n->server->bodies[i] = 0; n->server->copied[i] = 0;
    memset(n->server->sessions + i, 0, sizeof(n->server->sessions[i]));
}
static int release(RflNet *n, unsigned short i) {
    if (n->server->claimed[i] || !n->server->pool[i].accepted) return NET_BUSY;
    if (net_tcp_release(&n->server->listener, n->server->pool + i)) return NET_BUSY;
    rfl_server_clear(n, i); return NET_OK;
}
void rfl_server_service(RflNet *n) {
    NetTcp *tcp;
    unsigned short i;
    if (!n->server || !n->server->listener.net) return;
    net_tcp_listener_poll(&n->server->listener, n->now);
    /* Accept/parse at most the fixed pool capacity per service turn. */
    for (i = 0; i < NET_TCP_CONNECTIONS; ++i) {
        if (net_tcp_accept(&n->server->listener, &tcp) != NET_OK) break;
        net_http_server_start(n->server->sessions + (tcp - n->server->pool), tcp, n->now, 15000UL);
    }
    for (i = 0; i < n->server->listener.capacity; ++i) {
        if (!n->server->pool[i].accepted) continue;
        if (n->server->sessions[i].state) net_http_server_poll(n->server->sessions + i, n->now);
        if (n->server->sessions[i].state == NET_HTTP_SERVE_COMPLETE || n->server->sessions[i].state == NET_HTTP_SERVE_FAILED || !n->server->pool[i].active) release(n, i);
    }
}
unsigned short *rfl_http_listen(unsigned short *handle, unsigned short port, unsigned long sequence_seed, unsigned short *status) {
    RflNet *n = (RflNet *)handle;
    NetTcpListenConfig config;
    unsigned short i;
    *status = NET_CANCELLED;
    if (!n || n->stopped || !n->driver) return 0;
    if (!n->server) {
        n->server = calloc(1, sizeof(*n->server));
        if (!n->server) { *status = RFL_NET_MEMORY; return 0; }
        n->server_drop = rfl_server_drop;
    }
    *status = NET_BUSY;
    if (n->server->accepting || n->server->epoch == 0xffffffffUL) return 0;
    for (i = 0; i < NET_TCP_CONNECTIONS; ++i) if (n->server->claimed[i] || n->server->pool[i].accepted || n->server->pool[i].active || n->server->generations[i] == 0xffffffffUL) return 0;
    config.port = port; config.timeout_ms = 30000UL; config.sequence = sequence; config.user = n;
    *status = net_tcp_listen(&n->net, &n->server->listener, n->server->pool, NET_TCP_CONNECTIONS, &config);
    if (*status) return 0;
    *status = rfl_net_keep(n);
    if (*status) { net_tcp_unlisten(&n->server->listener); return 0; }
    n->server_service = rfl_server_service; n->server_drop = rfl_server_drop;
    n->serial = sequence_seed; n->server->accepting = 1; ++n->server->epoch; return handle;
}
unsigned long rfl_http_listener_epoch(unsigned short *handle) { return ((RflNet *)handle)->server->epoch; }
void rfl_http_unlisten(unsigned short *handle) {
    RflNet *n = (RflNet *)handle;
    unsigned short i;
    if (!n) return;
    n->server->accepting = 0; net_tcp_unlisten(&n->server->listener);
    for (i = 0; i < n->server->listener.capacity; ++i) if (n->server->pool[i].accepted && !n->server->claimed[i]) {
        net_http_server_cancel(n->server->sessions + i); release(n, i);
    }
    rfl_net_release(handle);
}
unsigned short *rfl_http_accept(unsigned short *handle, unsigned long epoch, unsigned short *status) {
    RflNet *n = (RflNet *)handle;
    RflIncoming *request;
    unsigned short i;
    *status = NET_CANCELLED;
    if (!n || n->stopped || !n->server) return 0;
    *status = NET_CLOSED;
    if (!n->server->accepting || !n->server->listener.active || n->server->epoch != epoch) return 0;
    for (i = 0; i < n->server->listener.capacity; ++i) {
        if (n->server->claimed[i] || n->server->sessions[i].state != NET_HTTP_SERVE_READY) continue;
        if (n->server->generations[i] == 0xffffffffUL) continue;
        request = calloc(1, sizeof(*request));
        if (!request) { *status = RFL_NET_MEMORY; return 0; }
        *status = rfl_net_keep(n);
        if (*status) { free(request); return 0; }
        n->server->claimed[i] = 1; ++n->server->generations[i];
        request->owner = n; request->slot = i; request->generation = n->server->generations[i];
        return (unsigned short *)request;
    }
    *status = NET_AGAIN; return 0;
}
static NetHttpServer *session(RflIncoming *r) {
    if (!r || !r->owner->server || r->owner->stopped || !r->owner->server->claimed[r->slot] || r->owner->server->generations[r->slot] != r->generation) return 0;
    return r->owner->server->sessions + r->slot;
}
unsigned short rfl_http_request_text(unsigned short *handle, unsigned short field, unsigned short *output) {
    NetHttpServer *s = session((RflIncoming *)handle);
    const char *text;
    unsigned short count = 0;
    if (!s) return 0;
    text = field == 0 ? s->request.method : field == 1 ? s->request.target : s->request.host;
    while (text[count] && count < 255) { output[count] = (unsigned char)text[count]; ++count; }
    return count;
}
unsigned short rfl_http_reply_begin(unsigned short *handle, unsigned short code, unsigned long length) {
    RflIncoming *r = (RflIncoming *)handle;
    NetHttpServer *s = session(r);
    unsigned char *body;
    int status;
    if (!s) return NET_CANCELLED;
    if (s->state != NET_HTTP_SERVE_READY || r->owner->server->bodies[r->slot]) return NET_BUSY;
    if (length > RFL_NET_BODY_LIMIT) return NET_HTTP_LIMIT;
    body = malloc(length ? length : 1);
    if (!body) return RFL_NET_MEMORY;
    /* Prepare immutable storage, but do not start transmission before the copy
     * is complete. A cancelled copy aborts the session through its owner. */
    status = net_http_server_reply(s, code, body, length);
    if (status) { free(body); return status; }
    s->state = NET_HTTP_SERVE_READY;
    r->owner->server->bodies[r->slot] = body; r->owner->server->copied[r->slot] = 0; return NET_OK;
}
unsigned short rfl_http_reply_copy(unsigned short *handle, unsigned short *bytes, unsigned short count) {
    RflIncoming *r = (RflIncoming *)handle;
    NetHttpServer *s = session(r);
    unsigned long at;
    unsigned short i;
    if (!s) return NET_CANCELLED;
    at = r->owner->server->copied[r->slot];
    if (!r->owner->server->bodies[r->slot] || s->state != NET_HTTP_SERVE_READY || count > 256 || count > s->length - at) return NET_BAD_ARGUMENT;
    for (i = 0; i < count; ++i) { if (bytes[i] > 255) return NET_BAD_ARGUMENT; r->owner->server->bodies[r->slot][at + i] = bytes[i]; }
    r->owner->server->copied[r->slot] += count; return NET_OK;
}
unsigned short rfl_http_reply_status(unsigned short *handle) {
    RflIncoming *r = (RflIncoming *)handle;
    NetHttpServer *s = session(r);
    if (!s) return NET_CANCELLED;
    if (s->state == NET_HTTP_SERVE_READY && r->owner->server->bodies[r->slot] && r->owner->server->copied[r->slot] == s->length) s->state = NET_HTTP_SERVE_SENDING;
    if (s->state == NET_HTTP_SERVE_COMPLETE) return NET_OK;
    if (s->state == NET_HTTP_SERVE_FAILED) return s->error;
    return NET_AGAIN;
}
void rfl_http_incoming_drop(unsigned short *handle) {
    RflIncoming *r = (RflIncoming *)handle;
    RflNet *n;
    unsigned short i;
    if (!r) return;
    n = r->owner; i = r->slot;
    if (n->server->claimed[i] && n->server->generations[i] == r->generation) {
        if (n->server->sessions[i].state != NET_HTTP_SERVE_COMPLETE) net_http_server_cancel(n->server->sessions + i);
        n->server->claimed[i] = 0; release(n, i);
    }
    free(r); rfl_net_release((unsigned short *)n);
}
