#include "http_internal.h"

/* 1=IPv4, 0=hostname, -1=malformed numeric address. */
int http_address(const char *host, unsigned char *address) {
    unsigned short i, part, value, digits;
    const char *p = host;
    for (i = 0; p[i]; ++i) if ((p[i] < '0' || p[i] > '9') && p[i] != '.') return 0;
    for (part = 0; part < 4; ++part) {
        value = 0; digits = 0;
        while (*p >= '0' && *p <= '9') {
            if (++digits > 3) return -1;
            value = value * 10 + *p++ - '0';
        }
        if (!digits || value > 255 || (part < 3 ? *p != '.' : *p != 0)) return -1;
        address[part] = value; if (part < 3) ++p;
    }
    return 1;
}
static void append(NetHttp *h, const char *s) {
    while (*s) h->request[h->request_length++] = *s++;
}
int http_request(NetHttp *h, const char *host, const char *path, unsigned short port) {
    unsigned short i, c, label = 0;
    if (!host || !*host || !path || *path != '/' || !port) return NET_BAD_ARGUMENT;
    for (i = 0; host[i]; ++i) {
        c = host[i];
        if (i >= 253) return NET_BAD_ARGUMENT;
        if (c == '.') {
            if (!label || host[i - 1] == '-') return NET_BAD_ARGUMENT;
            label = 0;
        } else {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) return NET_BAD_ARGUMENT;
            if ((!label && c == '-') || ++label > 63) return NET_BAD_ARGUMENT;
        }
    }
    if (host[i - 1] == '-') return NET_BAD_ARGUMENT;
    for (i = 0; path[i]; ++i) if (i >= 255 || path[i] <= 32 || path[i] >= 127 || path[i] == '#' || path[i] == '\\') return NET_BAD_ARGUMENT;
    h->request_length = 0;
    append(h, "GET "); append(h, path); append(h, " HTTP/1.0\r\nHost: "); append(h, host);
    if (port != 80) {
        append(h, ":");
        h->request_length += http_format_decimal(h->request + h->request_length, port);
    }
    append(h, "\r\nConnection: close\r\nAccept-Encoding: identity\r\n\r\n");
    h->request[h->request_length] = 0;
    return NET_OK;
}
