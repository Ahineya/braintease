#include "demo.h"
#include "http-page.h"
#include <net_dhcp.h>
#include <net_http_server.h>
#ifndef HTTP_SERVER_DHCP
#define HTTP_SERVER_DHCP 1
#endif
#ifndef HTTP_SERVER_PORT
#define HTTP_SERVER_PORT 8080
#endif
#ifndef HTTP_SERVER_TIMEOUT
#define HTTP_SERVER_TIMEOUT 5000UL
#endif
#ifndef HTTP_SERVER_LIMIT
#define HTTP_SERVER_LIMIT 0
#endif
static NetContext network;
static NetDhcp lease;
static NetTcpListener listener;
static NetTcp pool[2];
static NetTcpWaitTable closing;
static NetHttpServer session;
static unsigned long serial;
static unsigned long sequence(void *user, const unsigned char *peer, unsigned short port, unsigned long now) {
    /* Demonstration only. Production callers supply an unpredictable ISN. */
    return now * 250UL + ((unsigned long)network.mac[4] * 256UL + network.mac[5]) * 65536UL + ++serial * 64001UL;
}
static NetTcpListenConfig listen_config = {HTTP_SERVER_PORT, HTTP_SERVER_TIMEOUT * 3UL, sequence, 0};
static int network_poll(unsigned long now) {
#if HTTP_SERVER_DHCP
    return net_dhcp_poll(&lease, now);
#else
    return net_poll(&network, now);
#endif
}
int main(void) {
    VmClock clock;
    NetTcp *connection = 0;
    unsigned long served = 0;
    int status;
#if HTTP_SERVER_DHCP
    if (net_open(&network, 0, 0x4853) || vm_get_time(&clock) || net_dhcp_start(&network, &lease, clock.ms, 60000UL)) return 1;
    puts("HTTP SERVER DHCP acquiring");
    while (lease.state != NET_DHCP_BOUND) {
        if (network_poll(clock.ms) == NET_IO_ERROR || lease.state == NET_DHCP_FAILED || vm_next_tick(&clock)) return 2;
    }
#else
    if (net_init(&network, &config) || vm_get_time(&clock)) return 1;
#endif
    if (net_tcp_wait_attach(&network, &closing)) return 3;
    if (net_tcp_listen(&network, &listener, pool, 2, &listen_config)) return 3;
    text("HTTP SERVER ready http://"); ip_text(network.config.address); putchar(':'); number(HTTP_SERVER_PORT); puts("/");
    while (!HTTP_SERVER_LIMIT || served < HTTP_SERVER_LIMIT) {
        if (network_poll(clock.ms) == NET_IO_ERROR) return 4;
        status = net_tcp_listener_poll(&listener, clock.ms);
        if (status != NET_OK) { text("HTTP SERVER listener error="); number(status); putchar('\n'); return 5; }
        if (!connection) {
            status = net_tcp_accept(&listener, &connection);
            if (status != NET_OK && status != NET_AGAIN) return 6;
            if (connection && net_http_server_start(&session, connection, clock.ms, HTTP_SERVER_TIMEOUT)) return 6;
        }
        if (connection) {
            status = net_http_server_poll(&session, clock.ms);
            if (session.state == NET_HTTP_SERVE_READY) {
                if (strcmp(session.request.method, "GET")) status = net_http_server_reply(&session, 405, method_error, sizeof(method_error) - 1);
                else if (strcmp(session.request.target, "/")) status = net_http_server_reply(&session, 404, missing, sizeof(missing) - 1);
                else status = net_http_server_reply(&session, 200, page, sizeof(page) - 1);
                if (status != NET_OK) return 9;
            }
            if (status == NET_EOF || session.state == NET_HTTP_SERVE_FAILED) {
                status = net_tcp_release(&listener, connection);
                if (status == NET_OK) {
                    ++served; text("HTTP SERVER complete status="); number(session.status);
                    text(" sent="); number(session.sent); text(" error="); number(session.error);
                    text(" served="); number(served); putchar('\n'); connection = 0;
                } else if (status != NET_BUSY) return 7;
            }
        }
        if (vm_next_tick(&clock)) return 8;
    }
    net_tcp_unlisten(&listener); return 0;
}
