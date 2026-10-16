#include <net_http_server.h>
#include <stdio.h>
#include <string.h>
static NetHttpServer server;
static NetTcp tcp;
static unsigned char input[512], output[4096], body[713];
static unsigned short input_used, input_head, output_used, eof, blocked, close_stall, write_error;
static void check(int ok) { putchar(ok ? 'Y' : 'N'); }
int net_tcp_read(NetTcp *t, unsigned char *out, unsigned short cap, unsigned short *count) {
    unsigned short size = input_used - input_head;
    if (size > cap) size = cap; if (size > 3) size = 3;
    memcpy(out, input + input_head, size); *count = size; input_head += size;
    return size ? NET_OK : (eof ? NET_EOF : NET_AGAIN);
}
int net_tcp_write(NetTcp *t, const unsigned char *data, unsigned short length, unsigned short *count) {
    *count = 0; if (write_error) return write_error;
    if (blocked) return NET_AGAIN;
    if (length > 13) length = 13;
    memcpy(output + output_used, data, length); output_used += length; output[output_used] = 0; *count = length; return NET_OK;
}
int net_tcp_shutdown(NetTcp *t) { t->shutting = 1; t->state = NET_TCP_FIN_WAIT_1; return NET_OK; }
void net_tcp_abort(NetTcp *t) { if (t) { t->active = 0; t->state = NET_TCP_FAILED; } }
static void reset(const char *request) {
    unsigned short i;
    memset(&server, 0, sizeof(server)); memset(&tcp, 0, sizeof(tcp)); tcp.active = 1; tcp.state = NET_TCP_ESTABLISHED;
    strcpy((char *)input, request); input_used = strlen(request); input_head = 0; output_used = 0; output[0] = 0;
    eof = 1; blocked = 0; close_stall = 0; write_error = 0;
    for (i = 0; i < sizeof(body); ++i) body[i] = (i * 37U + 11U) % 256;
}
static int step(unsigned long now) {
    if (tcp.shutting && !close_stall) { tcp.state = NET_TCP_CLOSED; tcp.active = 0; }
    return net_http_server_poll(&server, now);
}
static int run(unsigned short reply) {
    unsigned long now, until = server.now + 3000UL;
    int status;
    for (now = server.now; now < until; ++now) {
        status = step(now);
        if (server.state == NET_HTTP_SERVE_READY) net_http_server_reply(&server, reply, body, sizeof(body));
        if (status == NET_EOF || server.state == NET_HTTP_SERVE_FAILED) return status;
    }
    return NET_AGAIN;
}
int main(void) {
    const char *header = "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=us-ascii\r\nContent-Length: 713\r\nConnection: close\r\n\r\n";
    unsigned short count;
    reset("GET / HTTP/1.1\r\nHost: guest.test\r\n\r\n");
    check(net_http_server_start(&server, &tcp, 0, 1000) == NET_OK);
    check(net_http_server_start(&server, &tcp, 0, 1000) == NET_BUSY);
    check(net_http_server_reply(&server, 200, body, sizeof(body)) == NET_BUSY);
    check(run(200) == NET_EOF && server.status == 200 && server.sent == sizeof(body));
    check(output_used == strlen(header) + sizeof(body) && !memcmp(output, header, strlen(header)) && !memcmp(output + strlen(header), body, sizeof(body)));
    /* Same pool address, a new connection: old session must not mistake this
     * newly active TCB for ownership of the previous completed connection. */
    tcp.active = 1; tcp.state = NET_TCP_ESTABLISHED; tcp.shutting = 0;
    check(net_http_server_start(&server, &tcp, 3000, 1000) == NET_OK);
    net_http_server_cancel(&server); check(server.error == NET_CANCELLED && !tcp.active);
    reset("GET / HTTP/1.1\r\n\r\n"); net_http_server_start(&server, &tcp, 0, 1000);
    check(run(200) == NET_EOF && server.status == 400 && !memcmp(output, "HTTP/1.0 400 Bad Request\r\n", 26));
    reset("POST / HTTP/1.0\r\n\r\n"); net_http_server_start(&server, &tcp, 0, 1000);
    check(run(405) == NET_EOF && server.status == 405);
    check(!memcmp(output + output_used - sizeof(body) - 14, "Allow: GET\r\n\r\n", 14));
    reset("GET / HTTP/1.1\r\nHost: guest"); net_http_server_start(&server, &tcp, 0, 1000);
    check(run(200) == NET_EOF && server.status == 400);
    reset("GET / HTTP/1.1\r\nHost: guest"); eof = 0; net_http_server_start(&server, &tcp, 0, 1000);
    check(run(200) == NET_EOF && server.status == 408);
    reset("GET / HTTP/1.0\r\n\r\n"); net_http_server_start(&server, &tcp, 0, 1000); blocked = 1;
    check(run(200) == NET_TIMEOUT && server.state == NET_HTTP_SERVE_FAILED && !tcp.active);
    reset("GET / HTTP/1.0\r\n\r\n"); net_http_server_start(&server, &tcp, 0, 1000); close_stall = 1;
    check(run(200) == NET_TIMEOUT && !tcp.active);
    reset("GET / HTTP/1.0\r\n\r\n"); net_http_server_start(&server, &tcp, 0, 1000); write_error = NET_RESET;
    check(run(200) == NET_RESET && !tcp.active);
    reset("GET / HTTP/1.1\r\nHost: slow.example.test\r\nX: slow"); eof = 0;
    net_http_server_start(&server, &tcp, 0, 1000);
    for (count = 0; count <= 10; ++count) step(count * 100UL);
    check(server.status == 408 && server.state == NET_HTTP_SERVE_SENDING);
    reset(""); eof = 0; net_http_server_start(&server, &tcp, 0, 1000);
    step(1000); check(server.status == 408 && server.state == NET_HTTP_SERVE_SENDING);
    reset(""); eof = 0; net_http_server_start(&server, &tcp, 0xfffffff0UL, 1000);
    step(984); check(server.status == 408);
    reset("GET / HTTP/1.0\r\n\r\n"); net_http_server_start(&server, &tcp, 0, 1000);
    for (count = 0; count < 20; ++count) step(count);
    check(server.state == NET_HTTP_SERVE_READY && net_http_server_reply(&server, 999, 0, 0) == NET_BAD_ARGUMENT);
    check(net_http_server_reply(&server, 200, 0, 1) == NET_BAD_ARGUMENT);
    check(net_http_server_reply(&server, 200, 0, 0) == NET_OK);
    check(run(200) == NET_EOF && !server.sent && server.length == 0);
    reset("GET / HTTP/1.0\r\n\r\n"); net_http_server_start(&server, &tcp, 0, 1000);
    server.state = NET_HTTP_SERVE_CLOSING; tcp.state = NET_TCP_TIME_WAIT;
    check(step(1) == NET_EOF && tcp.active);
    putchar('\n'); return 0;
}
