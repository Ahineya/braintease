/* HTTP orchestration against a deterministic stream, not another HTTP decoder.
 * Packet/TCP integration is exercised separately with an ordinary host server. */
#include <net_http.h>
#include <stdio.h>
#include <string.h>
static NetContext net;
static NetHttp http;
static NetHttpConfig config = {"192.168.64.1", "/hello?q=1", 8080, 123, 1000, 5000};
static char wire[1024], sent[1024];
static unsigned short wire_used, wire_head, sent_used, chunk, eof, stall, connect_stall, shutdowns, aborts, read_error;
static int dns_result;
int net_tcp_connect(NetContext *n, NetTcp *t, const NetTcpConfig *c, unsigned long now) {
    t->net = n; t->active = 1; t->state = NET_TCP_SYN_SENT; t->snd_nxt = c->initial_sequence;
    memcpy(t->peer, c->peer, 4); t->remote_port = c->remote_port; return NET_OK;
}
int net_tcp_poll(NetTcp *t, unsigned long now) {
    if (!connect_stall && t->state == NET_TCP_SYN_SENT) t->state = NET_TCP_ESTABLISHED;
    if (t->shutting && eof) { t->active = 0; t->state = NET_TCP_CLOSED; }
    return t->active ? NET_OK : NET_CLOSED;
}
int net_tcp_write(NetTcp *t, const unsigned char *data, unsigned short length, unsigned short *written) {
    *written = 0; if (stall) return NET_AGAIN;
    if (length > 17) length = 17;
    memcpy(sent + sent_used, data, length); sent_used += length; sent[sent_used] = 0; *written = length; return NET_OK;
}
int net_tcp_read(NetTcp *t, unsigned char *out, unsigned short cap, unsigned short *received) {
    unsigned short size = wire_used - wire_head;
    *received = 0; if (read_error) return read_error;
    if (size > cap) size = cap; if (size > chunk) size = chunk;
    if (size) { memcpy(out, wire + wire_head, size); wire_head += size; *received = size; return NET_OK; }
    return eof ? NET_EOF : NET_AGAIN;
}
int net_tcp_shutdown(NetTcp *t) { t->shutting = 1; ++shutdowns; return NET_OK; }
void net_tcp_abort(NetTcp *t) { if (t->active) ++aborts; t->active = 0; }
int net_dns_start(NetContext *n, NetDns *d, const char *name, unsigned long now, unsigned long timeout, unsigned short retries) {
    d->net = n; d->port = 1; d->status = NET_DNS_WAITING; strcpy(d->name, name); return NET_OK;
}
int net_dns_poll(NetDns *d, unsigned long now) {
    d->status = dns_result;
    if (dns_result != NET_DNS_WAITING) d->port = 0;
    d->address[0] = 192; d->address[1] = 168; d->address[2] = 64; d->address[3] = 1; return d->status;
}
void net_dns_cancel(NetDns *d) { if (d->port) { d->status = NET_DNS_CANCELLED; d->port = 0; } }
static void check(int ok) { putchar(ok ? 'Y' : 'N'); }
static void reset(const char *response) {
    memset(&http, 0, sizeof(http)); memset(&net, 0, sizeof(net)); net.link = 1;
    strcpy(wire, response); wire_used = strlen(wire); wire_head = 0; sent_used = 0; sent[0] = 0;
    chunk = 1; eof = 0; stall = 0; connect_stall = 0; shutdowns = 0; aborts = 0; read_error = 0; dns_result = NET_DNS_SUCCESS;
    config.host = "192.168.64.1"; config.path = "/hello?q=1"; config.port = 8080; config.timeout_ms = 1000; config.max_body = 5000;
}
static unsigned char output[1024];
static unsigned short received;
static int run(void) {
    unsigned long now;
    unsigned short count;
    int status = NET_AGAIN;
    received = 0;
    for (now = 0; now < 1100; ++now) {
        status = net_http_poll(&http, now);
        if (status != NET_OK && status != NET_AGAIN && status != NET_EOF) return status;
        status = net_http_read(&http, output + received, sizeof(output) - received, &count); received += count;
        if (status != NET_OK && status != NET_AGAIN) return status;
    }
    return status;
}
int main(void) {
    char big[257], long_path[256];
    unsigned char expected_peer[4] = {192, 168, 64, 1};
    unsigned short count;
    reset("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello");
    check(net_http_start(&net, &http, &config, 0) == NET_OK && http.state == NET_HTTP_CONNECTING);
    check(net_http_start(&net, &http, &config, 0) == NET_BUSY);
    check(run() == NET_EOF && received == 5 && !memcmp(output, "hello", 5));
    check(!strcmp(sent, "GET /hello?q=1 HTTP/1.0\r\nHost: 192.168.64.1:8080\r\nConnection: close\r\nAccept-Encoding: identity\r\n\r\n"));
    check(shutdowns == 1 && http.tcp.active);
    check(net_http_poll(&http, 3000) == NET_EOF && http.state == NET_HTTP_COMPLETE);
    net_http_cancel(&http); check(!http.tcp.active && aborts == 1 && net_http_read(&http, output, 1, &count) == NET_CANCELLED);
    reset("HTTP/1.0 404 Missing\r\n\r\nbody"); eof = 1;
    check(net_http_start(&net, &http, &config, 0) == NET_OK && run() == NET_EOF && received == 4 && http.response.status == 404);
    check(net_http_poll(&http, 500) == NET_EOF && !http.tcp.active);
    reset("HTTP/1.0 200 OK\r\nContent-Length: 5\r\n\r\nhi"); eof = 1;
    net_http_start(&net, &http, &config, 0); check(run() == NET_HTTP_TRUNCATED && received == 2 && !http.tcp.active);
    reset("HTTP/1.0 200 OK\r\nContent-Length: 5\r\n\r\nhi");
    net_http_start(&net, &http, &config, 0); check(run() == NET_TIMEOUT && received == 2 && aborts == 1);
    reset("HTTP/1.0 204 Empty\r\n\r\n"); config.host = "Example.test"; config.port = 80;
    check(net_http_start(&net, &http, &config, 0) == NET_OK && http.state == NET_HTTP_RESOLVING && !strcmp(http.dns.name, "Example.test"));
    check(run() == NET_EOF && !received && !memcmp(http.tcp.peer, expected_peer, 4));
    check(strchr(sent, ':') != 0 && !strcmp(http.request, "GET /hello?q=1 HTTP/1.0\r\nHost: Example.test\r\nConnection: close\r\nAccept-Encoding: identity\r\n\r\n"));
    reset(""); config.host = "missing.test"; dns_result = NET_DNS_NXDOMAIN;
    net_http_start(&net, &http, &config, 0); check(run() == NET_HTTP_DNS_ERROR && http.dns.status == NET_DNS_NXDOMAIN && !http.dns.port);
    reset(""); config.host = "slow.test"; dns_result = NET_DNS_WAITING;
    net_http_start(&net, &http, &config, 0); check(run() == NET_TIMEOUT && !http.dns.port && http.dns.status == NET_DNS_CANCELLED);
    reset(""); connect_stall = 1; net_http_start(&net, &http, &config, 0); check(run() == NET_TIMEOUT);
    reset(""); stall = 1; net_http_start(&net, &http, &config, 0); check(run() == NET_TIMEOUT && !sent_used);
    reset(""); read_error = NET_RESET; net_http_start(&net, &http, &config, 0); check(run() == NET_RESET);
    reset(""); net_http_start(&net, &http, &config, 0xfffffff0UL); check(net_http_poll(&http, 984UL) == NET_TIMEOUT);
    reset(""); config.host = "999.1.1.1"; check(net_http_start(&net, &http, &config, 0) == NET_BAD_ARGUMENT);
    reset(""); config.host = "1.2.3"; check(net_http_start(&net, &http, &config, 0) == NET_BAD_ARGUMENT);
    reset(""); config.host = "bad\r\nInjected: x"; check(net_http_start(&net, &http, &config, 0) == NET_BAD_ARGUMENT);
    reset(""); config.host = "-bad.test"; check(net_http_start(&net, &http, &config, 0) == NET_BAD_ARGUMENT);
    reset(""); config.path = "/bad\r\nX: y"; check(net_http_start(&net, &http, &config, 0) == NET_BAD_ARGUMENT);
    reset(""); config.path = "http://example.test/"; check(net_http_start(&net, &http, &config, 0) == NET_BAD_ARGUMENT);
    reset(""); config.path = "/fragment#x"; check(net_http_start(&net, &http, &config, 0) == NET_BAD_ARGUMENT);
    reset(""); memset(big, 'a', 256); big[0] = '/'; big[256] = 0; config.path = big;
    check(net_http_start(&net, &http, &config, 0) == NET_BAD_ARGUMENT);
    reset(""); big[255] = 0; config.path = big; check(net_http_start(&net, &http, &config, 0) == NET_OK);
    reset(""); config.port = 65535;
    check(net_http_start(&net, &http, &config, 0) == NET_OK && !strcmp(http.request, "GET /hello?q=1 HTTP/1.0\r\nHost: 192.168.64.1:65535\r\nConnection: close\r\nAccept-Encoding: identity\r\n\r\n"));
    reset(""); memset(big, 'a', 253); big[63] = '.'; big[127] = '.'; big[191] = '.'; big[253] = 0;
    memset(long_path, 'a', 255); long_path[0] = '/'; long_path[255] = 0;
    config.host = big; config.path = long_path; config.port = 65535;
    check(net_http_start(&net, &http, &config, 0) == NET_OK && http.request_length < NET_HTTP_REQUEST && !http.request[http.request_length]);
    reset(""); config.port = 0; check(net_http_start(&net, &http, &config, 0) == NET_BAD_ARGUMENT);
    reset(""); config.timeout_ms = 0; check(net_http_start(&net, &http, &config, 0) == NET_BAD_ARGUMENT);
    reset(""); net.link = 0; check(net_http_start(&net, &http, &config, 0) == NET_LINK_DOWN);
    reset("HTTP/1.0 200 OK\r\nContent-Length: 5001\r\n\r\n"); net_http_start(&net, &http, &config, 0); check(run() == NET_HTTP_LIMIT);
    reset("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"); net_http_start(&net, &http, &config, 0); check(run() == NET_UNSUPPORTED);
    reset("HTTP/1.0 200 OK\r\nContent-Length: 5\r\n\r\nhello"); chunk = 256;
    net_http_start(&net, &http, &config, 0);
    for (count = 0; count < 20; ++count) net_http_poll(&http, count);
    check(http.response.state == NET_HTTP_BODY && !http.response.received && http.used > http.head);
    check(net_http_poll(&http, 1000) == NET_TIMEOUT && !http.tcp.active);
    putchar('\n'); return 0;
}
