#include "http_internal.h"

static int fail(NetHttpParser *p, int status) {
    p->state = NET_HTTP_PARSE_FAILED; p->error = status; return status;
}
static int digit(unsigned short c) { return c >= '0' && c <= '9'; }
static int line(NetHttpParser *p) {
    char *value;
    unsigned short i;
    p->line[p->used] = 0;
    if (p->state == NET_HTTP_STATUS) {
        if (p->used < 13 || (memcmp(p->line, "HTTP/1.0 ", 9) && memcmp(p->line, "HTTP/1.1 ", 9)) ||
            !digit(p->line[9]) || !digit(p->line[10]) || !digit(p->line[11]) || p->line[12] != ' ')
            return fail(p, NET_HTTP_BAD_RESPONSE);
        p->status = (p->line[9] - '0') * 100 + (p->line[10] - '0') * 10 + p->line[11] - '0';
        if (p->status < 100 || p->status > 599) return fail(p, NET_HTTP_BAD_RESPONSE);
        if (p->status < 200) return fail(p, NET_UNSUPPORTED);
        p->state = NET_HTTP_FIELDS; return NET_OK;
    }
    if (!p->used) {
        if (p->status == 204 && p->has_length) return fail(p, NET_HTTP_BAD_RESPONSE);
        if (p->status == 204 || p->status == 304 || (p->has_length && !p->length)) p->state = NET_HTTP_DONE;
        else {
            if (p->has_length && p->length > p->max_body) return fail(p, NET_HTTP_LIMIT);
            p->state = NET_HTTP_BODY;
        }
        return NET_OK;
    }
    i = http_field(p, &value); if (i) return fail(p, i);
    /* No transfer/content decoding in v1. Never mistake chunk syntax or encoded
     * representation data for the requested identity body. */
    if (http_equal(p->line, "transfer-encoding")) return fail(p, NET_UNSUPPORTED);
    if (http_equal(p->line, "content-encoding") && !http_equal(value, "identity")) return fail(p, NET_UNSUPPORTED);
    if (http_equal(p->line, "content-length")) {
        if (p->has_length || !*value) return fail(p, NET_HTTP_BAD_RESPONSE);
        if (!http_decimal(value, &p->length)) return fail(p, NET_HTTP_BAD_RESPONSE);
        p->has_length = 1;
    }
    return NET_OK;
}
void net_http_parser_init(NetHttpParser *p, unsigned long max_body) {
    if (p) { memset(p, 0, sizeof(*p)); p->max_body = max_body; }
}
int net_http_parser_feed(NetHttpParser *p, const unsigned char *input, unsigned short length,
    unsigned short *consumed, unsigned char *output, unsigned short capacity, unsigned short *written) {
    unsigned short c;
    int status;
    if (!p || !consumed || !written || (length && !input) || (capacity && !output)) return NET_BAD_ARGUMENT;
    *consumed = 0; *written = 0;
    if (p->state == NET_HTTP_PARSE_FAILED) return p->error;
    while (*consumed < length && p->state != NET_HTTP_DONE) {
        if (p->state == NET_HTTP_BODY) {
            if (*written == capacity) break;
            if (p->received == p->max_body) return fail(p, NET_HTTP_LIMIT);
            c = input[*consumed]; if (c > 255) return fail(p, NET_HTTP_BAD_RESPONSE);
            output[(*written)++] = c; ++*consumed; ++p->received;
            if (p->has_length && p->received == p->length) p->state = NET_HTTP_DONE;
        } else {
            c = input[(*consumed)++];
            status = http_line_octet(p, c);
            if (status == NET_OK) { status = line(p); p->used = 0; if (status) return status; }
            else if (status != NET_AGAIN) return fail(p, status);
        }
    }
    return p->state == NET_HTTP_DONE ? NET_EOF : NET_AGAIN;
}
int net_http_parser_eof(NetHttpParser *p) {
    if (!p) return NET_BAD_ARGUMENT;
    if (p->state == NET_HTTP_PARSE_FAILED) return p->error;
    if (p->state == NET_HTTP_BODY && !p->has_length) p->state = NET_HTTP_DONE;
    return p->state == NET_HTTP_DONE ? NET_EOF : fail(p, NET_HTTP_TRUNCATED);
}
