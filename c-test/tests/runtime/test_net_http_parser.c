#include <net_http.h>
#include <stdio.h>
#include <string.h>
static NetHttpParser p;
static unsigned char out[512];
static unsigned short consumed, written;
static void check(int ok) { putchar(ok ? 'Y' : 'N'); }
static int feed(const char *text) {
    return net_http_parser_feed(&p, (const unsigned char *)text, strlen(text), &consumed, out, sizeof(out), &written);
}
static int decode(const char *text, unsigned long limit) { net_http_parser_init(&p, limit); return feed(text); }
int main(void) {
    unsigned short i, j, n, split, count, ok = 1;
    int status;
    const char *response = "HTTP/1.1 200 OK\r\ncOnTeNt-LeNgTh:\t5 \t\r\nX-Thing: yes\r\n\r\nhelloNEXT";
    n = strlen(response);
    for (split = 0; split <= n; ++split) {
        net_http_parser_init(&p, 10); count = 0;
        net_http_parser_feed(&p, (const unsigned char *)response, split, &consumed, out, 512, &written);
        count += written;
        if (consumed == split) {
            net_http_parser_feed(&p, (const unsigned char *)response + split, n - split, &consumed, out + count, 512 - count, &written);
            count += written;
        }
        if (count != 5 || memcmp(out, "hello", 5) || p.received != 5 || p.state != NET_HTTP_DONE || p.status != 200) ok = 0;
    }
    check(ok);
    net_http_parser_init(&p, 10); count = 0; i = 0;
    while (i < n && p.state != NET_HTTP_DONE) {
        status = net_http_parser_feed(&p, (const unsigned char *)response + i, 1, &consumed, out + count, 1, &written);
        i += consumed; count += written;
    }
    check(status == NET_EOF && count == 5 && !memcmp(out, "hello", 5) && i == n - 4);
    net_http_parser_init(&p, 10);
    status = net_http_parser_feed(&p, (const unsigned char *)response, n, &consumed, 0, 0, &written);
    check(status == NET_AGAIN && !written && p.state == NET_HTTP_BODY && consumed == n - 9);
    check(decode("HTTP/1.0 404 Missing\r\n\r\nbody", 4) == NET_AGAIN && written == 4 && p.status == 404);
    check(net_http_parser_eof(&p) == NET_EOF);
    check(decode("HTTP/1.0 200 OK\r\nContent-Length: 4\r\n\r\nabc", 4) == NET_AGAIN && written == 3);
    check(net_http_parser_eof(&p) == NET_HTTP_TRUNCATED);
    check(feed("d") == NET_HTTP_TRUNCATED);
    check(decode("HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\nextra", 0) == NET_EOF && !written && consumed == 38);
    check(decode("HTTP/1.0 204 No Content\r\n\r\n", 0) == NET_EOF);
    check(decode("HTTP/1.1 304 Not Modified\r\nContent-Length: 999999\r\n\r\n", 0) == NET_EOF);
    check(decode("HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n", 0) == NET_HTTP_BAD_RESPONSE);
    check(decode("HTTP/1.0 200 OK\r\nContent-Length: 5\r\n\r\n", 4) == NET_HTTP_LIMIT);
    check(decode("HTTP/1.0 200 OK\r\n\r\nabcde", 4) == NET_HTTP_LIMIT && written == 4);
    check(decode("HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n", 4) == NET_HTTP_BAD_RESPONSE);
    check(decode("HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n", 4) == NET_HTTP_BAD_RESPONSE);
    check(decode("HTTP/1.1 200 OK\r\nContent-Length: 1, 1\r\n\r\n", 4) == NET_HTTP_BAD_RESPONSE);
    check(decode("HTTP/1.1 200 OK\r\nContent-Length: +1\r\n\r\n", 4) == NET_HTTP_BAD_RESPONSE);
    check(decode("HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n", 4) == NET_HTTP_BAD_RESPONSE);
    check(decode("HTTP/1.1 200 OK\r\nContent-Length: \r\n\r\n", 4) == NET_HTTP_BAD_RESPONSE);
    check(decode("HTTP/1.1 200 OK\r\nContent-Length: 4294967296\r\n\r\n", 4) == NET_HTTP_BAD_RESPONSE);
    check(decode("HTTP/1.1 200 OK\r\nContent-Length: 4294967295\r\n\r\n", 4294967295UL) == NET_AGAIN && p.length == 4294967295UL);
    check(decode("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n", 4) == NET_UNSUPPORTED);
    check(decode("HTTP/1.1 200 OK\r\nContent-Length: 4\r\nTransfer-Encoding: identity\r\n\r\n", 4) == NET_UNSUPPORTED);
    check(decode("HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n\r\n", 4) == NET_UNSUPPORTED);
    check(decode("HTTP/1.1 200 OK\r\nContent-Encoding: Identity\r\nContent-Length: 0\r\n\r\n", 0) == NET_EOF);
    check(decode("HTTP/1.1 100 Continue\r\n\r\n", 4) == NET_UNSUPPORTED);
    check(decode("HTTP/2.0 200 OK\r\n\r\n", 4) == NET_HTTP_BAD_RESPONSE);
    check(decode("HTTP/1.1 20A OK\r\n\r\n", 4) == NET_HTTP_BAD_RESPONSE);
    check(decode("HTTP/1.1 600 OK\r\n\r\n", 4) == NET_HTTP_BAD_RESPONSE);
    check(decode("HTTP/1.1 200\r\n\r\n", 4) == NET_HTTP_BAD_RESPONSE);
    check(decode("HTTP/1.1 200 OK\n\n", 4) == NET_HTTP_BAD_RESPONSE);
    check(decode("HTTP/1.1 200 OK\rx", 4) == NET_HTTP_BAD_RESPONSE);
    check(decode("HTTP/1.1 200 OK\r\n folded\r\n\r\n", 4) == NET_HTTP_BAD_RESPONSE);
    check(decode("HTTP/1.1 200 OK\r\nName : value\r\n\r\n", 4) == NET_HTTP_BAD_RESPONSE);
    check(decode("HTTP/1.1 200 OK\r\n: value\r\n\r\n", 4) == NET_HTTP_BAD_RESPONSE);
    check(decode("HTTP/1.1 200 OK\r\nName: a\001b\r\n\r\n", 4) == NET_HTTP_BAD_RESPONSE);
    decode("HTTP/1.1 200 OK\r\nX:", 4); memset(out, 'a', 256);
    check(net_http_parser_feed(&p, out, 256, &consumed, 0, 0, &written) == NET_HTTP_LIMIT);
    decode("HTTP/1.1 200 OK\r\n", 4);
    for (i = 0; i < 64; ++i) feed("X:\r\n");
    check(feed("Y:\r\n") == NET_HTTP_LIMIT);
    decode("HTTP/1.1 200 OK\r\n", 4);
    for (i = 0; i < 20; ++i) { feed("X:"); for (j = 0; j < 240; ++j) feed("a"); feed("\r\n"); }
    check(p.error == NET_HTTP_LIMIT);
    decode("HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\n", 2); out[0] = 0; out[1] = 255;
    check(net_http_parser_feed(&p, out, 2, &consumed, out + 2, 2, &written) == NET_EOF && written == 2 && out[2] == 0 && out[3] == 255);
    decode("HTTP/1.0 200 OK\r\nX: unfinished", 4); check(net_http_parser_eof(&p) == NET_HTTP_TRUNCATED);
    net_http_parser_init(&p, 4); check(net_http_parser_eof(&p) == NET_HTTP_TRUNCATED);
    putchar('\n'); return 0;
}
