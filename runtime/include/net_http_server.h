#ifndef RIPPLE_NET_HTTP_SERVER_H
#define RIPPLE_NET_HTTP_SERVER_H
#include <net_http.h>
#define NET_HTTP_BAD_REQUEST 36
/* Bounded single-request decoder. No request-body or pipelining support. */
typedef struct NetHttpRequest {
    NetHttpParser header;
    char method[17], target[NET_HTTP_LINE], host[NET_HTTP_LINE];
    unsigned short version, hosts, status;
} NetHttpRequest;
void net_http_request_init(NetHttpRequest *request);
int net_http_request_feed(NetHttpRequest *request, const unsigned char *input,
    unsigned short length, unsigned short *consumed);
int net_http_request_eof(NetHttpRequest *request);

#define NET_HTTP_SERVE_READING 1
#define NET_HTTP_SERVE_READY 2
#define NET_HTTP_SERVE_SENDING 3
#define NET_HTTP_SERVE_CLOSING 4
#define NET_HTTP_SERVE_COMPLETE 5
#define NET_HTTP_SERVE_FAILED 6
typedef struct NetHttpServer {
    NetTcp *tcp;
    NetHttpRequest request;
    char response[NET_HTTP_LINE];
    unsigned char input[NET_HTTP_INPUT];
    const unsigned char *body;
    unsigned long length, sent, started, timeout, now;
    unsigned short state, error, status, header_length, header_sent;
} NetHttpServer;
/* Zero before first use. Network and listener/TCP poll drive transport first.
 * A session borrows its connection and never releases the listener pool itself. */
int net_http_server_start(NetHttpServer *server, NetTcp *tcp, unsigned long now_ms, unsigned long timeout_ms);
/* READY lets the app inspect method/target and choose a response. Parser errors
 * and deadlines produce bounded error responses automatically where possible. */
int net_http_server_poll(NetHttpServer *server, unsigned long now_ms);
/* One response, status 200/400/404/405/408/413/414/417/431/501/505, text/html.
 * Immutable body remains valid through completion/failure; writes are partial.
 * No automatic interpolation of request data into headers/HTML. */
int net_http_server_reply(NetHttpServer *server, unsigned short status,
    const unsigned char *body, unsigned long length);
/* EOF means closed or TIME_WAIT; release through net_tcp_release, retry BUSY.
 * Failure leaves the borrowed connection inactive for release. */
void net_http_server_cancel(NetHttpServer *server);
#endif
