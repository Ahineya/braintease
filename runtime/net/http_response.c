#include "http_internal.h"
#include <net_http_server.h>

static const char *reason(unsigned short status) {
    if (status == 200) return "OK";
    if (status == 400) return "Bad Request";
    if (status == 404) return "Not Found";
    if (status == 405) return "Method Not Allowed";
    if (status == 408) return "Request Timeout";
    if (status == 413) return "Content Too Large";
    if (status == 414) return "URI Too Long";
    if (status == 417) return "Expectation Failed";
    if (status == 431) return "Request Header Fields Too Large";
    if (status == 501) return "Not Implemented";
    if (status == 505) return "HTTP Version Not Supported";
    return 0;
}
static void append(NetHttpServer *s, const char *text) {
    while (*text) s->response[s->header_length++] = *text++;
}
int net_http_server_reply(NetHttpServer *s, unsigned short status, const unsigned char *body, unsigned long length) {
    const char *text = reason(status);
    if (!s || !text || (length && !body)) return NET_BAD_ARGUMENT;
    if (s->state != NET_HTTP_SERVE_READY) return NET_BUSY;
    s->header_length = 0; append(s, "HTTP/1.0 ");
    s->header_length += http_format_decimal(s->response + s->header_length, status);
    append(s, " "); append(s, text); append(s, "\r\nContent-Type: text/html; charset=us-ascii\r\nContent-Length: ");
    s->header_length += http_format_decimal(s->response + s->header_length, length);
    append(s, "\r\nConnection: close\r\n"); if (status == 405) append(s, "Allow: GET\r\n"); append(s, "\r\n");
    s->response[s->header_length] = 0; s->status = status; s->body = body; s->length = length;
    s->state = NET_HTTP_SERVE_SENDING; s->started = s->now; return NET_OK;
}
