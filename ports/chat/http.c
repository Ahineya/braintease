/* SPDX-License-Identifier: MIT */
#include "http.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <netdb.h>
#include <netutil.h>
#include <ripple_tls.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int fd;
    RippleTLS *tls;
    unsigned at, used;
    char input[2048];
    char *error;
    unsigned error_size;
    unsigned long started;
} Connection;
typedef struct {
    int fd;
    struct sockaddr_in peer;
} Dial;
static int connect_peer(void *context) {
    Dial *d = context;
    return nu_connect(d->fd, (const struct sockaddr *)&d->peer, sizeof(d->peer), 30000UL);
}
static int fail(Connection *c, const char *text) {
    snprintf(c->error, c->error_size, "%s", text);
    return -1;
}
static int byte(Connection *c) {
    long n;
    if (nu_expired(c->started, 300000UL))
        return fail(c, "HTTPS response exceeded five minutes");
    if (c->at == c->used) {
        n = ripple_tls_read(c->tls, c->input, sizeof(c->input), 60000UL);
        if (n < 0)
            return fail(c, ripple_tls_error());
        if (!n)
            return -2;
        c->at = 0;
        c->used = (unsigned)n;
    }
    return (unsigned char)c->input[c->at++];
}
static int line(Connection *c, char *out, unsigned limit, unsigned long *budget) {
    unsigned n = 0;
    int ch;
    for (;;) {
        ch = byte(c);
        if (ch < 0)
            return fail(c, "Truncated HTTPS headers or chunk framing");
        if (!*budget)
            return fail(c, "HTTPS headers exceed 16 KiB");
        --*budget;
        if (ch == '\r') {
            if (!*budget)
                return fail(c, "HTTPS header limit");
            --*budget;
            if (byte(c) != '\n')
                return fail(c, "Malformed HTTP line ending");
            out[n] = 0;
            return 0;
        }
        if (ch == '\n' || (ch < 32 && ch != '\t') || ch > 126 || n == limit)
            return fail(c, "Malformed or oversized HTTP header");
        out[n++] = ch;
    }
}
static int number(const char *p, unsigned base, unsigned long *value) {
    unsigned c, d, n = 0;
    *value = 0;
    while ((c = *p++) != 0) {
        if (base == 16 && c == ';')
            break;
        d = c >= '0' && c <= '9'   ? c - '0'
            : c >= 'a' && c <= 'f' ? c - 'a' + 10
            : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                   : 99;
        if (d >= base || *value > (0x7fffffffUL - d) / base)
            return 0;
        *value = *value * base + d;
        ++n;
    }
    return n != 0;
}
static int origin(const char *url, char *host, unsigned *port) {
    const char *p, *colon;
    unsigned long n;
    unsigned i;
    if (strncmp(url, "https://", 8) || !url[8])
        return 0;
    p = url + 8;
    colon = strchr(p, ':');
    n = colon ? (unsigned long)(colon - p) : strlen(p);
    if (!n || n > 253)
        return 0;
    for (i = 0; i < n; i++)
        if (!isalnum(p[i]) && p[i] != '.' && p[i] != '-')
            return 0;
    memcpy(host, p, n);
    host[n] = 0;
    *port = 443;
    if (colon) {
        if (!number(colon + 1, 10, &n) || !n || n > 65535UL)
            return 0;
        *port = (unsigned)n;
    }
    return 1;
}
static int deliver(Connection *c, unsigned long count, ChatBody receive, void *context) {
    unsigned take;
    while (count) {
        if (c->at == c->used) {
            int first = byte(c);
            if (first < 0)
                return fail(c, "Truncated HTTPS entity");
            --c->at;
        }
        take = c->used - c->at;
        if (take > count)
            take = (unsigned)count;
        if (receive(context, c->input + c->at, take))
            return -1;
        c->at += take;
        count -= take;
    }
    return 0;
}
int chat_http(const char *url, const char *path, const char *token, const char *content_type,
              FILE *body, const char *ca, ChatBody receive, void *context, char *error,
              unsigned error_size) {
    Connection c;
    Dial dial;
    struct hostent *h;
    struct sockaddr_in peer;
    char host[254], header[1025], *p, *end, request[1024];
    unsigned port, interim = 0, chunked = 0, length_seen = 0, encoding = 0;
    int status = -1, ch, result = -1;
    unsigned long length = 0, chunk, budget = 16384UL, total = 0;
    long size = 0;
    memset(&c, 0, sizeof(c));
    c.fd = -1;
    c.error = error;
    c.error_size = error_size;
    c.started = nu_now();
    error[0] = 0;
    if (!origin(url, host, &port))
        return fail(&c, "HTTPS is required; origin must be https://HOST[:PORT]");
    if (path[0] != '/' || strlen(path) > 512)
        return fail(&c, "Invalid HTTPS request path");
    for (p = (char *)path; *p; p++)
        if (*p <= 32 || *p > 126)
            return fail(&c, "Invalid HTTPS request path");
    if (token)
        for (p = (char *)token; *p; p++)
            if (*p <= 32 || *p > 126)
                return fail(&c, "Invalid bearer credential");
    if (body) {
        if (fseek(body, 0, SEEK_END) || (size = ftell(body)) < 0 || size > 4194304L ||
            fseek(body, 0, SEEK_SET))
            return fail(&c, "Request exceeds 4 MiB or cannot be read");
    }
    h = gethostbyname(host);
    if (!h)
        return fail(&c, "DNS lookup failed");
    memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    peer.sin_port = htons(port);
    memcpy(&peer.sin_addr, h->h_addr_list[0], sizeof(peer.sin_addr));
    c.fd = socket(AF_INET, SOCK_STREAM, 0);
    if (c.fd < 0) {
        fail(&c, "TCP connection failed");
        goto done;
    }
    dial.fd = c.fd;
    dial.peer = peer;
    c.tls = ripple_tls_open_dial(c.fd, host, ca, 120000UL, connect_peer, &dial);
    if (!c.tls) {
        fail(&c, ripple_tls_error());
        goto done;
    }
    snprintf(
        request, sizeof(request),
        "%s %s HTTP/1.1\r\nHost: %s:%u\r\nUser-Agent: Fuse-Chat/0.1\r\nAccept: text/event-stream, "
        "application/json\r\nAccept-Encoding: identity\r\nConnection: close\r\n",
        body ? "POST" : "GET", path, host, port);
    if (ripple_tls_write(c.tls, request, strlen(request), 30000UL))
        goto io;
    if (token) {
        if (ripple_tls_write(c.tls, "Authorization: Bearer ", 22, 30000UL) ||
            ripple_tls_write(c.tls, token, strlen(token), 30000UL) ||
            ripple_tls_write(c.tls, "\r\n", 2, 30000UL))
            goto io;
    }
    if (body) {
        snprintf(request, sizeof(request), "Content-Type: %s\r\nContent-Length: %ld\r\n",
                 content_type, size);
        if (ripple_tls_write(c.tls, request, strlen(request), 30000UL))
            goto io;
    }
    if (ripple_tls_write(c.tls, "\r\n", 2, 30000UL))
        goto io;
    if (body) {
        size_t n;
        while ((n = fread(request, 1, sizeof(request), body)) != 0)
            if (ripple_tls_write(c.tls, request, n, 30000UL))
                goto io;
        if (ferror(body)) {
            fail(&c, "Cannot read request body");
            goto done;
        }
    }
headers:
    if (line(&c, header, 1024, &budget))
        goto done;
    if (strncmp(header, "HTTP/1.1 ", 9) && strncmp(header, "HTTP/1.0 ", 9)) {
        fail(&c, "Invalid HTTP status line");
        goto done;
    }
    if (strlen(header) < 12 || header[9] < '1' || header[9] > '5' || !isdigit(header[10]) ||
        !isdigit(header[11]) || (header[12] && header[12] != ' ')) {
        fail(&c, "Invalid HTTP status code");
        goto done;
    }
    status = (header[9] - '0') * 100 + (header[10] - '0') * 10 + header[11] - '0';
    chunked = length_seen = encoding = 0;
    while (1) {
        if (line(&c, header, 1024, &budget))
            goto done;
        if (!header[0])
            break;
        p = strchr(header, ':');
        if (!p || p == header) {
            fail(&c, "Malformed HTTP header");
            goto done;
        }
        {
            char *q;
            for (q = header; q < p; q++)
                if (!isalnum(*q) && !strchr("!#$%&'*+-.^_`|~", *q)) {
                    fail(&c, "Invalid HTTP header name");
                    goto done;
                }
        }
        *p++ = 0;
        while (*p == ' ' || *p == '\t')
            p++;
        end = p + strlen(p);
        while (end > p && (end[-1] == ' ' || end[-1] == '\t'))
            *--end = 0;
        if (!strcasecmp(header, "Content-Length")) {
            if (length_seen++ || !number(p, 10, &length)) {
                fail(&c, "Invalid or duplicate Content-Length");
                goto done;
            }
        } else if (!strcasecmp(header, "Transfer-Encoding")) {
            if (chunked++ || strcasecmp(p, "chunked")) {
                fail(&c, "Unsupported Transfer-Encoding");
                goto done;
            }
        } else if (!strcasecmp(header, "Content-Encoding")) {
            if (encoding++ || strcasecmp(p, "identity")) {
                fail(&c, "Unsupported Content-Encoding");
                goto done;
            }
        }
    }
    if (status >= 100 && status < 200) {
        if (status == 101 || ++interim > 4) {
            fail(&c, "Unsupported HTTP upgrade");
            goto done;
        }
        goto headers;
    }
    if (chunked && length_seen) {
        fail(&c, "Ambiguous HTTP entity framing");
        goto done;
    }
    if (status != 200) {
        snprintf(error, error_size, "HTTP %d%s", status,
                 status == 401                   ? ": sign in with ChatGPT again"
                 : status == 429                 ? ": usage or rate limit reached"
                 : status >= 300 && status < 400 ? ": redirects are not followed"
                                                 : "");
        result = status;
        goto done;
    }
    if (chunked) {
        for (;;) {
            budget = 16384UL;
            if (line(&c, header, 1024, &budget) || !number(header, 16, &chunk)) {
                if (!*error)
                    fail(&c, "Invalid chunk size");
                goto done;
            }
            if (!chunk) {
                while (1) {
                    if (line(&c, header, 1024, &budget))
                        goto done;
                    if (!*header)
                        break;
                }
                break;
            }
            if (chunk > 8388608UL - total) {
                fail(&c, "HTTPS response exceeds 8 MiB");
                goto done;
            }
            total += chunk;
            if (deliver(&c, chunk, receive, context))
                goto done;
            if (byte(&c) != '\r' || byte(&c) != '\n') {
                fail(&c, "Invalid chunk terminator");
                goto done;
            }
        }
    } else if (length_seen) {
        if (length > 8388608UL) {
            fail(&c, "HTTPS response exceeds 8 MiB");
            goto done;
        }
        if (deliver(&c, length, receive, context))
            goto done;
    } else {
        while ((ch = byte(&c)) >= 0) {
            --c.at;
            chunk = c.used - c.at;
            if (chunk > 8388608UL - total) {
                fail(&c, "HTTPS response exceeds 8 MiB");
                goto done;
            }
            total += chunk;
            if (deliver(&c, chunk, receive, context))
                goto done;
        }
        if (ch != -2)
            goto done;
    }
    result = 200;
    goto done;
io:
    fail(&c, ripple_tls_error());
done:
    if (c.tls)
        ripple_tls_close(c.tls);
    if (c.fd >= 0)
        close(c.fd);
    return result;
}
