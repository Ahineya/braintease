/* All HTTP, DNS and TCP run in guest C; the host supplies Ethernet only. */
#include "demo.h"
#include <net_http.h>
#include <net_dhcp.h>
#ifndef HTTP_DHCP
#define HTTP_DHCP 1
#endif
#ifndef HTTP_HOST
#define HTTP_HOST "192.168.64.1"
#endif
#ifndef HTTP_PATH
#define HTTP_PATH "/"
#endif
#ifndef HTTP_PORT
#define HTTP_PORT 8080
#endif
#ifndef HTTP_TIMEOUT_MS
#define HTTP_TIMEOUT_MS 30000UL
#endif
#ifndef HTTP_MAX_BODY
#define HTTP_MAX_BODY 1048576UL
#endif
#ifndef HTTP_PRINT_BODY
#define HTTP_PRINT_BODY 1
#endif
static NetContext network;
static NetDhcp lease;
static NetHttp http;
static NetHttpConfig request = {HTTP_HOST, HTTP_PATH, HTTP_PORT, 0, HTTP_TIMEOUT_MS, HTTP_MAX_BODY};
static unsigned char body[173];
static int network_poll(unsigned long now) {
#if HTTP_DHCP
    return net_dhcp_poll(&lease, now);
#else
    return net_poll(&network, now);
#endif
}
static int error(int status) {
    text("HTTP ERROR status="); number(status); text(" dns="); number(http.dns.status);
    text(" received="); number(http.response.received); putchar('\n');
    net_http_cancel(&http); return 1;
}
int main(void) {
    VmClock clock;
    VmNetStats stats;
    unsigned short count, i, headers = 0, eof = 0;
    unsigned long received = 0, checksum = 0, close_started;
    int status;
#if HTTP_DHCP
    if (net_open(&network, 0, 0x4847) || vm_get_time(&clock) || net_dhcp_start(&network, &lease, clock.ms, 60000UL)) return error(NET_IO_ERROR);
    puts("HTTP DHCP acquiring");
    while (lease.state != NET_DHCP_BOUND) {
        if (network_poll(clock.ms) == NET_IO_ERROR || lease.state == NET_DHCP_FAILED) return error(NET_IO_ERROR);
        if (vm_next_tick(&clock)) return error(NET_IO_ERROR);
    }
#else
    if (net_init(&network, &config) || vm_get_time(&clock)) return error(NET_IO_ERROR);
#endif
    text("HTTP local="); ip_text(network.config.address); text(" GET http://" HTTP_HOST ":"); number(HTTP_PORT); puts(HTTP_PATH);
    /* Demonstration entropy only. Supply an unpredictable TCP ISN on untrusted networks. */
    request.initial_sequence = clock.ms * 250UL + ((unsigned long)network.mac[4] * 256UL + network.mac[5]) * 65536UL;
    status = net_http_start(&network, &http, &request, clock.ms); if (status) return error(status);
    while (!eof) {
        if (network_poll(clock.ms) == NET_IO_ERROR) return error(NET_IO_ERROR);
        status = net_http_poll(&http, clock.ms);
        if (status != NET_OK && status != NET_AGAIN && status != NET_EOF) return error(status);
        if (!headers && http.response.state >= NET_HTTP_BODY) {
            headers = 1; text("HTTP response status="); number(http.response.status);
            text(" length="); if (http.response.has_length) number(http.response.length); else text("close-delimited");
            puts("\nHTTP BODY");
        }
        do {
            status = net_http_read(&http, body, sizeof(body), &count);
            for (i = 0; i < count; ++i) {
                checksum += body[i];
#if HTTP_PRINT_BODY
                putchar(body[i]);
#endif
            }
            received += count;
            if (status == NET_EOF) eof = 1;
            else if (status != NET_OK && status != NET_AGAIN) return error(status);
        } while (status == NET_OK);
        if (vm_next_tick(&clock)) return error(NET_IO_ERROR);
    }
    /* The body is complete independently of TCP teardown. Normally the HTTP/1.0
     * server closes first. Bound this demo's wait; explicit cancellation below
     * gives up TIME_WAIT if a server leaves the connection open. */
    close_started = clock.ms;
    while (http.tcp.active && clock.ms - close_started < 3000UL) {
        if (network_poll(clock.ms) == NET_IO_ERROR) return error(NET_IO_ERROR);
        net_http_poll(&http, clock.ms);
        if (vm_next_tick(&clock)) return error(NET_IO_ERROR);
    }
    if (http.tcp.active) net_http_cancel(&http);
    do {
        if (network_poll(clock.ms) == NET_IO_ERROR || vm_net_stats(0, &stats) || stats.tx_failed || stats.backend_errors) return error(NET_IO_ERROR);
        if (vm_next_tick(&clock)) return error(NET_IO_ERROR);
    } while (network.pending || stats.tx_sent != stats.tx_accepted);
    text("\nHTTP END\nHTTP OK status="); number(http.response.status); text(" received="); number(received);
    text(" checksum="); number(checksum); putchar('\n');
    counters(&stats); return 0;
}
