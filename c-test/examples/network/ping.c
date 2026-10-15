/* Ethernet, ARP, IPv4 and ICMP live in libruntime; this is only a ping client. */
#include "demo.h"
#ifndef NET_TARGET_IP
#define NET_TARGET_IP 192,168,64,1
#endif
#ifndef PING_REQUESTS
#define PING_REQUESTS 1
#endif
#ifndef PING_RESPONSES
#define PING_RESPONSES 1
#endif
#ifndef PING_MONITOR
#define PING_MONITOR 0
#endif
static unsigned char target_ip[4] = {NET_TARGET_IP};
static NetContext net;
int main(void) {
    VmNetStats stats;
    VmClock clock;
    unsigned char payload[17];
    unsigned short i, sent = 0, received = 0;
    int status;
    if (net_init(&net, &config)) return fail("NIC unavailable or invalid configuration", 2);
    for (i = 0; i < 17; ++i) payload[i] = i == 1 ? 255 : i;
#if PING_MONITOR
    text("PING monitor local="); ip_text(config.address); text(" mask="); ip_text(config.mask); putchar('\n');
    text("gateway="); ip_text(config.gateway); text(" MAC=");
    for (i = 0; i < 6; ++i) {
        if (i) putchar(':');
        putchar("0123456789abcdef"[net.mac[i] / 16]);
        putchar("0123456789abcdef"[net.mac[i] % 16]);
    }
    puts("\nWaiting for pings; Ctrl-C to stop.");
#else
    text("PING ready local="); ip_text(config.address); text(" target="); ip_text(target_ip); putchar('\n');
#endif
    while (PING_MONITOR || received < PING_REQUESTS || net.icmp_requests < PING_RESPONSES || net.pending) {
        if (vm_get_time(&clock)) return fail("clock unavailable", 3);
        status = net_poll(&net, clock.ms);
        if (status == NET_IO_ERROR) return fail("network polling failed", 4);
#if PING_MONITOR
        if (net.event == NET_EVENT_ECHO_REQUEST) {
            text("PING request from="); ip_text(net.peer); text(" id="); number(net.identifier);
            text(" seq="); number(net.sequence); text(" octets="); number(net.payload_length); putchar('\n');
            puts(net.pending ? "PING reply waiting for TX space" : "PING reply queued");
        }
#endif
        if (net.event == NET_EVENT_ECHO_REPLY && !memcmp(net.peer, target_ip, 4) && net.identifier == 0x5246) {
            if (net.payload_length != 17 || net.sequence != received + 1 || net.sequence > sent || memcmp(net.payload, payload, 17)) ++net.rejected;
            else { ++received; text("PING reply seq="); number(net.sequence); puts(" octets=17"); }
        }
        if (sent < PING_REQUESTS && received == sent) {
            status = net_icmp_echo(&net, target_ip, 0x5246, sent + 1, payload, 17);
            if (status == NET_OK) ++sent;
            else if (status != NET_AGAIN) return fail("echo request failed", 5);
        }
    }
    do {
        if (vm_net_stats(0, &stats) || stats.tx_failed || stats.backend_errors) return fail("backend failed while draining TX", 9);
    } while (stats.tx_sent != stats.tx_accepted);
    text("PING OK received="); number(received); text(" responded="); number(net.icmp_requests);
    text(" rejected="); number(net.rejected); text(" drops="); number(stats.rx_dropped); putchar('\n');
    counters(&stats);
    return 0;
}
