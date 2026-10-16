#include "http_internal.h"

int http_fail(NetHttp *h, int error) {
    net_dns_cancel(&h->dns); net_tcp_abort(&h->tcp);
    h->state = NET_HTTP_FAILED; h->error = error; return error;
}
void net_http_cancel(NetHttp *h) {
    if (h) { net_dns_cancel(&h->dns); net_tcp_abort(&h->tcp); h->used = 0; h->head = 0; h->state = NET_HTTP_FAILED; h->error = NET_CANCELLED; }
}
int net_http_start(NetContext *net, NetHttp *h, const NetHttpConfig *config, unsigned long now_ms) {
    int status;
    if (!net || !h || !config || !config->timeout_ms || config->timeout_ms > 600000UL) return NET_BAD_ARGUMENT;
    if (h->tcp.active || h->dns.port || (h->state && h->state != NET_HTTP_COMPLETE && h->state != NET_HTTP_FAILED)) return NET_BUSY;
    if (!net->link) return NET_LINK_DOWN;
    memset(h, 0, sizeof(*h));
    status = http_request(h, config->host, config->path, config->port);
    if (status) return status;
    h->net = net; h->started = now_ms; h->timeout = config->timeout_ms;
    h->remote.remote_port = config->port; h->remote.timeout_ms = config->timeout_ms;
    h->remote.initial_sequence = config->initial_sequence;
    net_http_parser_init(&h->response, config->max_body);
    status = http_address(config->host, h->remote.peer);
    if (status < 0) return http_fail(h, NET_BAD_ARGUMENT);
    if (status) {
        status = net_tcp_connect(net, &h->tcp, &h->remote, now_ms);
        if (status) return http_fail(h, status);
        h->state = NET_HTTP_CONNECTING;
    } else {
        status = net_dns_start(net, &h->dns, config->host, now_ms, h->timeout < 3000UL ? h->timeout : 3000UL, 2);
        if (status) return http_fail(h, status);
        h->state = NET_HTTP_RESOLVING;
    }
    return NET_OK;
}
static void complete(NetHttp *h) {
    h->state = NET_HTTP_COMPLETE;
    if (h->tcp.active) net_tcp_shutdown(&h->tcp);
}
int net_http_read(NetHttp *h, unsigned char *output, unsigned short capacity, unsigned short *received) {
    unsigned short consumed = 0;
    int status;
    if (!h || !received || (capacity && !output)) return NET_BAD_ARGUMENT;
    *received = 0;
    if (h->state == NET_HTTP_FAILED) return h->error;
    if (h->state == NET_HTTP_COMPLETE) return NET_EOF;
    if (h->state != NET_HTTP_RESPONSE) return NET_AGAIN;
    status = net_http_parser_feed(&h->response, h->input + h->head, h->used - h->head, &consumed, output, capacity, received);
    h->head += consumed;
    if (status == NET_EOF) { complete(h); return NET_EOF; }
    if (status != NET_AGAIN) return http_fail(h, status);
    return *received ? NET_OK : NET_AGAIN;
}
int net_http_poll(NetHttp *h, unsigned long now_ms) {
    unsigned short count;
    int status;
    if (!h || !h->net || !h->state) return NET_BAD_ARGUMENT;
    if (h->state == NET_HTTP_FAILED) return h->error;
    if (h->state == NET_HTTP_COMPLETE) {
        if (h->tcp.active) {
            status = net_tcp_poll(&h->tcp, now_ms);
            if (status != NET_OK && status != NET_AGAIN && status != NET_CLOSED) h->cleanup_error = status;
        }
        return NET_EOF;
    }
    /* Total deadline includes DNS, handshake, response and caller backpressure.
     * Progress cannot keep a slow/infinite response alive indefinitely. */
    if (now_ms - h->started >= h->timeout) return http_fail(h, NET_TIMEOUT);
    if (h->state == NET_HTTP_RESOLVING) {
        status = net_dns_poll(&h->dns, now_ms);
        if (status == NET_DNS_WAITING) return NET_AGAIN;
        if (status != NET_DNS_SUCCESS) return http_fail(h, NET_HTTP_DNS_ERROR);
        memcpy(h->remote.peer, h->dns.address, 4);
        status = net_tcp_connect(h->net, &h->tcp, &h->remote, now_ms);
        if (status) return http_fail(h, status);
        h->state = NET_HTTP_CONNECTING;
    }
    status = net_tcp_poll(&h->tcp, now_ms);
    if (status != NET_OK && status != NET_AGAIN && status != NET_CLOSED) return http_fail(h, status);
    if (h->state == NET_HTTP_CONNECTING && (h->tcp.state == NET_TCP_ESTABLISHED || h->tcp.state == NET_TCP_CLOSE_WAIT)) h->state = NET_HTTP_SENDING;
    if (h->state == NET_HTTP_SENDING) {
        status = net_tcp_write(&h->tcp, (const unsigned char *)h->request + h->sent, h->request_length - h->sent, &count);
        if (status != NET_OK && status != NET_AGAIN) return http_fail(h, status);
        h->sent += count;
        if (h->sent == h->request_length) h->state = NET_HTTP_RESPONSE;
    }
    if (h->state != NET_HTTP_RESPONSE) return NET_AGAIN;
    if (h->head == h->used) {
        h->head = 0; h->used = 0;
        status = net_tcp_read(&h->tcp, h->input, NET_HTTP_INPUT, &count);
        h->used = count;
        if (status == NET_EOF) {
            status = net_http_parser_eof(&h->response);
            if (status != NET_EOF) return http_fail(h, status);
            complete(h); return NET_EOF;
        }
        if (status != NET_OK && status != NET_AGAIN) return http_fail(h, status);
    }
    /* Parse headers without requiring an application body buffer. */
    return net_http_read(h, 0, 0, &count);
}
