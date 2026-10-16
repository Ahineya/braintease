#include "http_internal.h"
#include <net_http_server.h>

static const unsigned char rejected[] = "<!doctype html><html><body><h1>Request rejected</h1></body></html>\n";
static int fail(NetHttpServer *s, int error) {
    net_tcp_abort(s->tcp); s->state = NET_HTTP_SERVE_FAILED; s->error = error; return error;
}
void net_http_server_cancel(NetHttpServer *s) { if (s) fail(s, NET_CANCELLED); }
int net_http_server_start(NetHttpServer *s, NetTcp *tcp, unsigned long now_ms, unsigned long timeout_ms) {
    if (!s || !tcp || !timeout_ms || timeout_ms > 600000UL) return NET_BAD_ARGUMENT;
    if (s->state && s->state != NET_HTTP_SERVE_COMPLETE && s->state != NET_HTTP_SERVE_FAILED) return NET_BUSY;
    if (!tcp->active || (tcp->state != NET_TCP_ESTABLISHED && tcp->state != NET_TCP_CLOSE_WAIT)) return NET_CLOSED;
    memset(s, 0, sizeof(*s)); s->tcp = tcp; s->started = now_ms; s->now = now_ms;
    s->timeout = timeout_ms; s->state = NET_HTTP_SERVE_READING; return NET_OK;
}
static int reject(NetHttpServer *s, unsigned short status) {
    s->state = NET_HTTP_SERVE_READY;
    return net_http_server_reply(s, status, rejected, sizeof(rejected) - 1);
}
int net_http_server_poll(NetHttpServer *s, unsigned long now_ms) {
    unsigned short count, consumed;
    int status;
    if (!s || !s->tcp || !s->state) return NET_BAD_ARGUMENT;
    if (s->state == NET_HTTP_SERVE_FAILED) return s->error;
    if (s->state == NET_HTTP_SERVE_COMPLETE) return NET_EOF;
    s->now = now_ms;
    if (s->state == NET_HTTP_SERVE_CLOSING && (s->tcp->state == NET_TCP_CLOSED || s->tcp->state == NET_TCP_TIME_WAIT)) {
        s->state = NET_HTTP_SERVE_COMPLETE; return NET_EOF;
    }
    if (!s->tcp->active || s->tcp->state == NET_TCP_FAILED) return fail(s, s->tcp->error ? s->tcp->error : NET_CLOSED);
    if (now_ms - s->started >= s->timeout) {
        if (s->state == NET_HTTP_SERVE_READING || s->state == NET_HTTP_SERVE_READY) return reject(s, 408);
        return fail(s, NET_TIMEOUT);
    }
    if (s->state == NET_HTTP_SERVE_READING) {
        status = net_tcp_read(s->tcp, s->input, sizeof(s->input), &count);
        if (status != NET_OK && status != NET_AGAIN && status != NET_EOF) return fail(s, status);
        if (status == NET_EOF) status = net_http_request_eof(&s->request);
        else status = net_http_request_feed(&s->request, s->input, count, &consumed);
        if (status == NET_EOF) s->state = NET_HTTP_SERVE_READY;
        else if (status != NET_AGAIN) return reject(s, s->request.status);
        return NET_AGAIN;
    }
    if (s->state == NET_HTTP_SERVE_READY) return NET_OK;
    /* Drain unwanted extra input without extending any deadline. The server
     * closes after one response; it never treats pipelined bytes as a new request. */
    status = net_tcp_read(s->tcp, s->input, sizeof(s->input), &count);
    if (status != NET_OK && status != NET_AGAIN && status != NET_EOF) return fail(s, status);
    if (s->state == NET_HTTP_SERVE_SENDING) {
        if (s->header_sent < s->header_length) {
            status = net_tcp_write(s->tcp, (const unsigned char *)s->response + s->header_sent, s->header_length - s->header_sent, &count);
            if (status != NET_OK && status != NET_AGAIN) return fail(s, status);
            s->header_sent += count;
        } else if (s->sent < s->length) {
            count = s->length - s->sent > NET_TCP_BUFFER ? NET_TCP_BUFFER : s->length - s->sent;
            status = net_tcp_write(s->tcp, s->body + s->sent, count, &count);
            if (status != NET_OK && status != NET_AGAIN) return fail(s, status);
            s->sent += count;
        } else {
            status = net_tcp_shutdown(s->tcp); if (status) return fail(s, status);
            s->state = NET_HTTP_SERVE_CLOSING; s->started = now_ms;
        }
    }
    return NET_AGAIN;
}
