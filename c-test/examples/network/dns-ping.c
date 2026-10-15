/* Resolve the name inside the guest, then ping that address. Host provides
 * Ethernet only. NEXT_TICK must be paced for real-network timeout semantics. */
#include "demo.h"
#include <net_dns.h>
#ifndef DNS_NAME
#define DNS_NAME "syma-lang.org"
#endif
#ifndef DNS_TIMEOUT_MS
#define DNS_TIMEOUT_MS 3000UL
#endif
#ifndef PING_REQUESTS
#define PING_REQUESTS 3
#endif
static NetContext net;
static NetDns dns;
int main(void) {
    VmClock clock;
    VmNetStats stats;
    unsigned char payload[17];
    unsigned short i, sent = 0, received = 0;
    unsigned long last = 0;
    int status;
    if (net_init(&net, &config)) return fail("NIC unavailable or invalid configuration", 1);
    if (vm_get_time(&clock)) return fail("clock unavailable", 2);
    text("DNS query name=" DNS_NAME " local="); ip_text(config.address); text(" server="); ip_text(config.dns); putchar('\n');
    if (net_dns_start(&net, &dns, DNS_NAME, clock.ms, DNS_TIMEOUT_MS, 2)) return fail("invalid DNS configuration or name", 3);
    while (dns.status == NET_DNS_WAITING) {
        if (net_poll(&net, clock.ms) == NET_IO_ERROR) return fail("network polling failed", 4);
        net_dns_poll(&dns, clock.ms);
        if (dns.status == NET_DNS_WAITING && vm_next_tick(&clock)) return fail("clock tick failed", 5);
    }
    if (dns.status != NET_DNS_SUCCESS) { text("DNS failed status="); number(dns.status); text(" malformed="); number(dns.malformed); putchar('\n'); return 6; }
    text("DNS answer name=" DNS_NAME " address="); ip_text(dns.address); text(" ttl="); number(dns.ttl); putchar('\n');
    for (i = 0; i < 17; ++i) payload[i] = i == 1 ? 255 : i;
    while (received < PING_REQUESTS) {
        if (net_poll(&net, clock.ms) == NET_IO_ERROR) return fail("network polling failed", 7);
        if (net.event == NET_EVENT_ECHO_REPLY && !memcmp(net.peer, dns.address, 4) && net.identifier == 0x5246 &&
            net.sequence == received + 1 && net.sequence <= sent && net.payload_length == 17 && !memcmp(net.payload, payload, 17)) {
            ++received; text("PING reply seq="); number(net.sequence); puts(" octets=17");
        }
        if (sent < PING_REQUESTS && received == sent) {
            status = net_icmp_echo(&net, dns.address, 0x5246, sent + 1, payload, 17);
            if (status == NET_OK) { ++sent; last = clock.ms; }
            else if (status != NET_AGAIN) return fail("echo send failed", 8);
        }
        if (sent > received && clock.ms - last >= 5000UL) return fail("echo reply timed out", 9);
        if (vm_next_tick(&clock)) return fail("clock tick failed", 10);
    }
    do {
        if (net_poll(&net, clock.ms) == NET_IO_ERROR || vm_net_stats(0, &stats) || stats.tx_failed || stats.backend_errors) return fail("TX drain failed", 11);
        if (vm_next_tick(&clock)) return fail("clock tick failed", 12);
    } while (net.pending || stats.tx_sent != stats.tx_accepted);
    text("PING OK received="); number(received); text(" responded="); number(net.icmp_requests);
    text(" rejected="); number(net.rejected); text(" drops="); number(stats.rx_dropped); putchar('\n');
    counters(&stats);
    return 0;
}
