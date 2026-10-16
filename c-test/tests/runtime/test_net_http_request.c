#include <net_http_server.h>
#include <stdio.h>
#include <string.h>
static NetHttpRequest r;
static unsigned short consumed;
static unsigned char big[300];
static void check(int ok) { putchar(ok ? 'Y' : 'N'); }
static int feed(const char *text) { return net_http_request_feed(&r, (const unsigned char *)text, strlen(text), &consumed); }
static int parse(const char *text) { net_http_request_init(&r); return feed(text); }
int main(void) {
    const char *text = "GET /hello?q=1 HTTP/1.1\r\nhOsT:\tguest.test:8080 \t\r\nContent-Length: 0\r\n\r\nNEXT";
    unsigned short n = strlen(text), split, total, i, j, ok = 1;
    int status;
    for (split = 0; split <= n; ++split) {
        net_http_request_init(&r);
        status = net_http_request_feed(&r, (const unsigned char *)text, split, &consumed); total = consumed;
        if (consumed == split) { status = net_http_request_feed(&r, (const unsigned char *)text + split, n - split, &consumed); total += consumed; }
        if (status != NET_EOF || total != n - 4 || strcmp(r.method, "GET") || strcmp(r.target, "/hello?q=1") || strcmp(r.host, "guest.test:8080") || r.version != 11) ok = 0;
    }
    check(ok);
    net_http_request_init(&r); i = 0;
    while (i < n && r.header.state != NET_HTTP_DONE) { status = net_http_request_feed(&r, (const unsigned char *)text + i, 1, &consumed); i += consumed; }
    check(status == NET_EOF && i == n - 4);
    check(parse("GET / HTTP/1.0\r\n\r\n") == NET_EOF && r.version == 10 && !r.hosts);
    check(parse("POST / HTTP/1.1\r\nHost: guest\r\nContent-Length: 0\r\n\r\n") == NET_EOF && !strcmp(r.method, "POST"));
    check(parse("GET / HTTP/1.1\r\n\r\n") == NET_HTTP_BAD_REQUEST && r.status == 400);
    check(parse("GET / HTTP/1.1\r\nHost: \r\n\r\n") == NET_HTTP_BAD_REQUEST);
    check(parse("GET / HTTP/1.1\r\nHost: guest\r\nHost: guest\r\n\r\n") == NET_HTTP_BAD_REQUEST);
    check(parse("GET / HTTP/1.1\r\nHost: bad host\r\n\r\n") == NET_HTTP_BAD_REQUEST);
    check(parse("GET / HTTP/1.1\r\nHost: guest:65536\r\n\r\n") == NET_HTTP_BAD_REQUEST);
    check(parse("GET / HTTP/1.1\r\nHost: [::1]\r\n\r\n") == NET_HTTP_BAD_REQUEST);
    check(parse("GET / HTTP/1.1\r\nHost: guest:65535\r\n\r\n") == NET_EOF);
    check(parse("GET / HTTP/2.0\r\n\r\n") == NET_UNSUPPORTED && r.status == 505);
    check(parse("GET / HTTP/1.1 extra\r\nHost: guest\r\n\r\n") == NET_HTTP_BAD_REQUEST && r.status == 400);
    check(parse("GET http://guest/ HTTP/1.1\r\nHost: guest\r\n\r\n") == NET_HTTP_BAD_REQUEST);
    check(parse("GET /#fragment HTTP/1.1\r\nHost: guest\r\n\r\n") == NET_HTTP_BAD_REQUEST);
    check(parse("GET /path\t HTTP/1.1\r\nHost: guest\r\n\r\n") == NET_HTTP_BAD_REQUEST);
    check(parse("GET  / HTTP/1.1\r\nHost: guest\r\n\r\n") == NET_HTTP_BAD_REQUEST);
    check(parse("G@T / HTTP/1.1\r\nHost: guest\r\n\r\n") == NET_HTTP_BAD_REQUEST);
    check(parse("GET / HTTP/1.1\nHost: guest\n\n") == NET_HTTP_BAD_REQUEST);
    check(parse("GET / HTTP/1.1\rX") == NET_HTTP_BAD_REQUEST);
    check(parse("GET / HTTP/1.1\r\nHost : guest\r\n\r\n") == NET_HTTP_BAD_REQUEST);
    check(parse("GET / HTTP/1.1\r\nHost: guest\r\n folded\r\n\r\n") == NET_HTTP_BAD_REQUEST);
    check(parse("GET / HTTP/1.1\r\nHost: guest\r\nX: a\001b\r\n\r\n") == NET_HTTP_BAD_REQUEST);
    check(parse("GET / HTTP/1.1\r\nHost: guest\r\nContent-Length: -1\r\n\r\n") == NET_HTTP_BAD_REQUEST);
    check(parse("GET / HTTP/1.1\r\nHost: guest\r\nContent-Length: 4294967296\r\n\r\n") == NET_HTTP_BAD_REQUEST);
    check(parse("GET / HTTP/1.1\r\nHost: guest\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n") == NET_HTTP_BAD_REQUEST);
    check(parse("GET / HTTP/1.1\r\nHost: guest\r\nContent-Length: 1\r\n\r\nx") == NET_UNSUPPORTED && r.status == 413);
    check(parse("GET / HTTP/1.1\r\nHost: guest\r\nTransfer-Encoding: chunked\r\n\r\n") == NET_UNSUPPORTED && r.status == 501);
    check(parse("GET / HTTP/1.1\r\nHost: guest\r\nExpect: 100-continue\r\n\r\n") == NET_UNSUPPORTED && r.status == 417);
    net_http_request_init(&r); memset(big, 'a', 300);
    check(net_http_request_feed(&r, big, 300, &consumed) == NET_HTTP_LIMIT && r.status == 414);
    parse("GET / HTTP/1.0\r\nX:");
    check(net_http_request_feed(&r, big, 300, &consumed) == NET_HTTP_LIMIT && r.status == 431);
    parse("GET / HTTP/1.0\r\n"); for (i = 0; i < 64; ++i) feed("X:\r\n");
    check(feed("Y:\r\n") == NET_HTTP_LIMIT && r.status == 431);
    parse("GET / HTTP/1.0\r\n");
    for (i = 0; i < 20; ++i) { feed("X:"); for (j = 0; j < 240; ++j) feed("a"); feed("\r\n"); }
    check(r.header.error == NET_HTTP_LIMIT && r.status == 431);
    parse("GET / HTTP/1.1\r\nHost:"); check(net_http_request_eof(&r) == NET_HTTP_TRUNCATED && r.status == 400);
    check(feed("guest\r\n\r\n") == NET_HTTP_TRUNCATED);
    net_http_request_init(&r); check(net_http_request_eof(&r) == NET_HTTP_TRUNCATED);
    parse("GET / HTTP/1.0\r\n\r\n"); check(net_http_request_eof(&r) == NET_EOF);
    putchar('\n'); return 0;
}
