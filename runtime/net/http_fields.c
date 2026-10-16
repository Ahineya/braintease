#include "http_internal.h"

/* Strict bounded CRLF/field parsing shared by client responses and requests. */
int http_line_octet(NetHttpParser *p, unsigned short c) {
    if (++p->headers > NET_HTTP_HEADERS) return NET_HTTP_LIMIT;
    if (p->cr) {
        if (c != '\n') return NET_HTTP_BAD_RESPONSE;
        p->cr = 0; p->line[p->used] = 0; return NET_OK;
    }
    if (c == '\r') p->cr = 1;
    else {
        if ((c < 32 && c != '\t') || c == 127 || c > 255) return NET_HTTP_BAD_RESPONSE;
        if (p->used == NET_HTTP_LINE - 1) return NET_HTTP_LIMIT;
        p->line[p->used++] = c;
    }
    return NET_AGAIN;
}
int http_field(NetHttpParser *p, char **value) {
    unsigned short i = 0;
    char *end;
    if (++p->fields > 64) return NET_HTTP_LIMIT;
    while (i < p->used && http_token(p->line[i])) ++i;
    if (!i || p->line[i] != ':') return NET_HTTP_BAD_RESPONSE;
    p->line[i] = 0; *value = p->line + i + 1;
    while (**value == ' ' || **value == '\t') ++*value;
    end = p->line + p->used;
    while (end > *value && (end[-1] == ' ' || end[-1] == '\t')) --end;
    *end = 0; return NET_OK;
}
