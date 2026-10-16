#ifndef RIPPLE_NET_HTTP_H
#define RIPPLE_NET_HTTP_H
#include <net_dns.h>
#include <net_tcp.h>

#define NET_HTTP_LINE 256
#define NET_HTTP_HEADERS 4096
#define NET_HTTP_REQUEST 768
#define NET_HTTP_INPUT 256
#define NET_HTTP_BAD_RESPONSE 32
#define NET_HTTP_LIMIT 33
#define NET_HTTP_TRUNCATED 34
#define NET_HTTP_DNS_ERROR 35
#define NET_HTTP_STATUS 0
#define NET_HTTP_FIELDS 1
#define NET_HTTP_BODY 2
#define NET_HTTP_DONE 3
#define NET_HTTP_PARSE_FAILED 4

/* Incremental GET response decoder, independent of TCP. No allocation. */
typedef struct NetHttpParser {
    char line[NET_HTTP_LINE];
    unsigned short state, error, status, used, cr, headers, fields, has_length;
    unsigned long length, received, max_body;
} NetHttpParser;
void net_http_parser_init(NetHttpParser *parser, unsigned long max_body);
/* Consumes headers even with output capacity zero. Stops at a full output buffer
 * or the message boundary. Inspect consumed; following bytes belong to another
 * message. EOF means a complete body, not a TCP EOF. Errors are sticky. */
int net_http_parser_feed(NetHttpParser *parser, const unsigned char *input,
    unsigned short length, unsigned short *consumed, unsigned char *output,
    unsigned short capacity, unsigned short *written);
int net_http_parser_eof(NetHttpParser *parser);

#define NET_HTTP_IDLE 0
#define NET_HTTP_RESOLVING 1
#define NET_HTTP_CONNECTING 2
#define NET_HTTP_SENDING 3
#define NET_HTTP_RESPONSE 4
#define NET_HTTP_COMPLETE 5
#define NET_HTTP_FAILED 6

typedef struct NetHttpConfig {
    /* Hostname or dotted IPv4, no scheme/port. Origin-form path, starting '/'.
     * Copied during start; host <=253 and path <=255 ASCII characters. */
    const char *host;
    const char *path;
    unsigned short port;
    unsigned long initial_sequence, timeout_ms, max_body;
} NetHttpConfig;
/* Zero before first use. Stable caller-owned storage while DNS/TCP is active.
 * Call network/DHCP poll first, then HTTP poll, then read repeatedly until AGAIN.
 * A completed body may still have an active TCP close/TIME_WAIT: keep polling
 * this object until tcp.active==0, or explicitly cancel to relinquish protection.
 * body EOF remains successful even if subsequent TCP cleanup fails. */
typedef struct NetHttp {
    NetContext *net;
    NetDns dns;
    NetTcp tcp;
    NetHttpParser response;
    NetTcpConfig remote;
    char request[NET_HTTP_REQUEST];
    unsigned char input[NET_HTTP_INPUT];
    unsigned short state, error, cleanup_error, request_length, sent, head, used;
    unsigned long started, timeout;
} NetHttp;
int net_http_start(NetContext *net, NetHttp *http, const NetHttpConfig *config, unsigned long now_ms);
int net_http_poll(NetHttp *http, unsigned long now_ms);
/* Partial binary body reads. Status is in response.status once headers complete.
 * A non-2xx HTTP status is still a valid response, and its body is readable.
 * Consume *received even when this call also returns EOF or an error. */
int net_http_read(NetHttp *http, unsigned char *output, unsigned short capacity, unsigned short *received);
/* Explicit cancellation, including TCP TIME_WAIT. Discards unread body. */
void net_http_cancel(NetHttp *http);
#endif
