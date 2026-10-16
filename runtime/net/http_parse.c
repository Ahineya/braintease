#include <net_http.h>
#include <string.h>

static int fail(NetHttpParser *p, int status) {
    p->state = NET_HTTP_PARSE_FAILED; p->error = status; return status;
}
static int digit(unsigned short c) { return c >= '0' && c <= '9'; }
static int token(unsigned short c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || digit(c) ||
        (c && strchr("!#$%&'*+-.^_`|~", c) != 0);
}
static int equal(const char *a, const char *b) {
    unsigned short c;
    while (*a && *b) {
        c = *a++; if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != *b++) return 0;
    }
    return !*a && !*b;
}
static int line(NetHttpParser *p) {
    char *value, *end;
    unsigned short i;
    unsigned long length = 0;
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
    if (++p->fields > 64) return fail(p, NET_HTTP_LIMIT);
    i = 0; while (i < p->used && token(p->line[i])) ++i;
    if (!i || p->line[i] != ':') return fail(p, NET_HTTP_BAD_RESPONSE);
    p->line[i] = 0; value = p->line + i + 1;
    while (*value == ' ' || *value == '\t') ++value;
    end = p->line + p->used;
    while (end > value && (end[-1] == ' ' || end[-1] == '\t')) --end;
    *end = 0;
    /* No transfer/content decoding in v1. Never mistake chunk syntax or encoded
     * representation data for the requested identity body. */
    if (equal(p->line, "transfer-encoding")) return fail(p, NET_UNSUPPORTED);
    if (equal(p->line, "content-encoding") && !equal(value, "identity")) return fail(p, NET_UNSUPPORTED);
    if (equal(p->line, "content-length")) {
        if (p->has_length || !*value) return fail(p, NET_HTTP_BAD_RESPONSE);
        while (*value) {
            if (!digit(*value)) return fail(p, NET_HTTP_BAD_RESPONSE);
            i = *value++ - '0';
            if (length > 429496729UL || (length == 429496729UL && i > 5)) return fail(p, NET_HTTP_BAD_RESPONSE);
            length = length * 10UL + i;
        }
        p->has_length = 1; p->length = length;
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
            if (++p->headers > NET_HTTP_HEADERS) return fail(p, NET_HTTP_LIMIT);
            if (p->cr) {
                if (c != '\n') return fail(p, NET_HTTP_BAD_RESPONSE);
                p->cr = 0; status = line(p); p->used = 0;
                if (status) return status;
            } else if (c == '\r') p->cr = 1;
            else {
                if ((c < 32 && c != '\t') || c == 127 || c > 255) return fail(p, NET_HTTP_BAD_RESPONSE);
                if (p->used == NET_HTTP_LINE - 1) return fail(p, NET_HTTP_LIMIT);
                p->line[p->used++] = c;
            }
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
