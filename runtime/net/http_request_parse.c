#include "http_internal.h"
#include <net_http_server.h>

static int fail(NetHttpRequest *r, int error, unsigned short status) {
    r->header.state = NET_HTTP_PARSE_FAILED; r->header.error = error; r->status = status; return error;
}
static int request_line(NetHttpRequest *r) {
    char *s = r->header.line, *target, *version;
    unsigned short i;
    target = strchr(s, ' '); if (!target) return fail(r, NET_HTTP_BAD_REQUEST, 400);
    *target++ = 0; i = 0; while (s[i] && http_token(s[i])) ++i;
    if (!i || s[i] || i >= sizeof(r->method)) return fail(r, NET_HTTP_BAD_REQUEST, 400);
    version = strchr(target, ' '); if (!version) return fail(r, NET_HTTP_BAD_REQUEST, 400);
    *version++ = 0;
    if (*target != '/') return fail(r, NET_HTTP_BAD_REQUEST, 400);
    for (i = 0; target[i]; ++i) if (target[i] <= 32 || target[i] >= 127 || target[i] == '#' || target[i] == '\\') return fail(r, NET_HTTP_BAD_REQUEST, 400);
    if (strlen(version) != 8 || memcmp(version, "HTTP/", 5) || version[5] < '0' || version[5] > '9' ||
        version[6] != '.' || version[7] < '0' || version[7] > '9') return fail(r, NET_HTTP_BAD_REQUEST, 400);
    if (!strcmp(version, "HTTP/1.1")) r->version = 11;
    else if (!strcmp(version, "HTTP/1.0")) r->version = 10;
    else return fail(r, NET_UNSUPPORTED, 505);
    strcpy(r->method, s); strcpy(r->target, target); r->header.state = NET_HTTP_FIELDS; return NET_OK;
}
static int host_valid(const char *value) {
    unsigned short i = 0, label = 0;
    unsigned long port;
    if (!*value) return 0;
    while (value[i] && value[i] != ':') {
        if (value[i] == '.') { if (!label || value[i - 1] == '-') return 0; label = 0; }
        else {
            if (!((value[i] >= 'a' && value[i] <= 'z') || (value[i] >= 'A' && value[i] <= 'Z') ||
                (value[i] >= '0' && value[i] <= '9') || value[i] == '-')) return 0;
            if ((!label && value[i] == '-') || ++label > 63) return 0;
        }
        ++i;
    }
    if (!i || value[i - 1] == '-' || i > 253) return 0;
    if (value[i] && (!http_decimal(value + i + 1, &port) || !port || port > 65535UL)) return 0;
    return 1;
}
static int field(NetHttpRequest *r) {
    NetHttpParser *p = &r->header;
    char *value;
    int status;
    if (!p->used) {
        if (r->version == 11 && !r->hosts) return fail(r, NET_HTTP_BAD_REQUEST, 400);
        if (p->has_length && p->length) return fail(r, NET_UNSUPPORTED, 413);
        p->state = NET_HTTP_DONE; return NET_OK;
    }
    status = http_field(p, &value);
    if (status) return fail(r, status == NET_HTTP_LIMIT ? status : NET_HTTP_BAD_REQUEST, status == NET_HTTP_LIMIT ? 431 : 400);
    if (http_equal(p->line, "host")) {
        if (r->hosts++ || !host_valid(value)) return fail(r, NET_HTTP_BAD_REQUEST, 400);
        strcpy(r->host, value);
    } else if (http_equal(p->line, "content-length")) {
        if (p->has_length || !http_decimal(value, &p->length)) return fail(r, NET_HTTP_BAD_REQUEST, 400);
        p->has_length = 1;
    } else if (http_equal(p->line, "transfer-encoding")) return fail(r, NET_UNSUPPORTED, 501);
    else if (http_equal(p->line, "expect")) return fail(r, NET_UNSUPPORTED, 417);
    return NET_OK;
}
void net_http_request_init(NetHttpRequest *r) { if (r) memset(r, 0, sizeof(*r)); }
int net_http_request_feed(NetHttpRequest *r, const unsigned char *input, unsigned short length, unsigned short *consumed) {
    int status;
    unsigned short initial;
    if (!r || !consumed || (length && !input)) return NET_BAD_ARGUMENT;
    *consumed = 0;
    if (r->header.state == NET_HTTP_PARSE_FAILED) return r->header.error;
    while (*consumed < length && r->header.state != NET_HTTP_DONE) {
        initial = r->header.state;
        status = http_line_octet(&r->header, input[(*consumed)++]);
        if (status == NET_OK) {
            status = initial == NET_HTTP_STATUS ? request_line(r) : field(r); r->header.used = 0;
            if (status) return status;
        } else if (status != NET_AGAIN) return fail(r, status == NET_HTTP_LIMIT ? status : NET_HTTP_BAD_REQUEST,
            status == NET_HTTP_LIMIT ? (initial == NET_HTTP_STATUS ? 414 : 431) : 400);
    }
    return r->header.state == NET_HTTP_DONE ? NET_EOF : NET_AGAIN;
}
int net_http_request_eof(NetHttpRequest *r) {
    if (!r) return NET_BAD_ARGUMENT;
    if (r->header.state == NET_HTTP_PARSE_FAILED) return r->header.error;
    return r->header.state == NET_HTTP_DONE ? NET_EOF : fail(r, NET_HTTP_TRUNCATED, 400);
}
