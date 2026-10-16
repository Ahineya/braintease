/* Real guest packet paths with a fake NIC, independent checksums and clock. */
#include <net_dns.h>
#include <net_dhcp.h>
#include <stdio.h>
#include <string.h>
static NetContext net;
static NetDns dns;
static NetDhcp dhcp;
static NetConfig config = {0, {192,168,64,42}, {255,255,255,0}, {192,168,64,1}, {192,168,64,1}, 7};
static unsigned char incoming[600], outgoing[600], old_quote[28];
static unsigned short in_length, out_length, block_tx, connected = 1, mac_tail = 1, transmissions, callbacks;
static unsigned char peer[4] = {192,168,64,1}, byte = 7;
unsigned short vm_net_info(unsigned short id, VmNetInfo *info) {
    memset(info, 0, sizeof(*info)); info->link = connected; info->mtu = 1500; info->max_frame = 1514;
    info->mac[0] = 2; info->mac[5] = mac_tail; return 0;
}
unsigned short vm_net_tx(unsigned short id, const unsigned char *p, unsigned short length, unsigned short *outcome) {
    if (!connected) { *outcome = VM_NET_LINK_DOWN; return 0; }
    if (block_tx) { *outcome = VM_NET_WOULD_BLOCK; return 0; }
    *outcome = VM_NET_OK; memcpy(outgoing, p, length); out_length = length; ++transmissions; return 0;
}
unsigned short vm_net_rx(unsigned short id, unsigned char *p, unsigned short cap, VmNetReceive *result) {
    result->outcome = !connected ? VM_NET_LINK_DOWN : (in_length ? VM_NET_OK : VM_NET_EMPTY);
    result->length = connected ? in_length : 0;
    if (connected) memcpy(p, incoming, in_length);
    in_length = 0; return 0;
}
static void check(int ok) { putchar(ok ? 'Y' : 'N'); }
static unsigned short read16(const unsigned char *p) { return p[0] * 256U + p[1]; }
static void put16(unsigned char *p, unsigned short v) { p[0] = v / 256U; p[1] = v % 256U; }
static unsigned short checksum(const unsigned char *p, unsigned short n) {
    unsigned long sum = 0;
    while (n >= 2) { sum += read16(p); p += 2; n -= 2; }
    if (n) sum += p[0] * 256UL;
    while (sum > 65535UL) sum = (sum & 65535UL) + sum / 65536UL;
    return 65535UL - sum;
}
static void reset(void) {
    connected = 1; mac_tail = 1; block_tx = 0; in_length = 0; transmissions = 0;
    memset(&dns, 0, sizeof(dns)); memset(&dhcp, 0, sizeof(dhcp));
    net_init(&net, &config);
}
static void arp(unsigned short tail) {
    memset(incoming, 0, 60); memcpy(incoming, net.mac, 6); incoming[6] = 2; incoming[11] = tail;
    put16(incoming + 12, 0x0806); put16(incoming + 14, 1); put16(incoming + 16, 0x0800);
    incoming[18] = 6; incoming[19] = 4; put16(incoming + 20, 2);
    incoming[22] = 2; incoming[27] = tail; memcpy(incoming + 28, peer, 4);
    memcpy(incoming + 32, net.mac, 6); memcpy(incoming + 38, net.config.address, 4); in_length = 60;
}
static int send(void) { return net_udp_send(&net, peer, 9000, 9001, &byte, 1); }
static void learn(void) { send(); arp(2); net_poll(&net, net.now + 1); }
static void receive(NetContext *ctx, const NetDatagram *packet, void *user) { ++callbacks; }
static void receive_error(NetContext *ctx, const NetError *error, void *user) { ++callbacks; }
static void fix_error(void) {
    put16(incoming + 24, 0); put16(incoming + 24, checksum(incoming + 14, 20));
    put16(incoming + 36, 0); put16(incoming + 36, checksum(incoming + 34, in_length - 34));
}
static void fix_quote(void) {
    put16(incoming + 52, 0); put16(incoming + 52, checksum(incoming + 42, 20));
    fix_error();
}
static void error(unsigned short type, unsigned short code) {
    memset(incoming, 0, sizeof(incoming)); memcpy(incoming, net.mac, 6); incoming[6] = 2; incoming[11] = 2;
    put16(incoming + 12, 0x0800); incoming[14] = 0x45; put16(incoming + 16, 56);
    incoming[22] = 64; incoming[23] = 1; memcpy(incoming + 26, peer, 4); memcpy(incoming + 30, net.config.address, 4);
    incoming[34] = type; incoming[35] = code;
    if (type == 3 && code == 4) put16(incoming + 40, 1400);
    memcpy(incoming + 42, outgoing + 14, 28); in_length = 70; fix_error();
}
static void query(unsigned long timeout, unsigned short retries) {
    net_dns_start(&net, &dns, "example.org", net.now, timeout, retries); net_dns_poll(&dns, net.now);
}
static void bounded_resources(void) {
    unsigned short i, used = 0;
    reset(); learn();
    for (i = 0; i <= NET_TX_HISTORY; ++i) {
        send(); if (!i) memcpy(old_quote, outgoing + 14, 28);
    }
    error(3, 3); memcpy(incoming + 42, old_quote, 28); fix_error(); net_poll(&net, 2);
    check(net.counters.icmp_unmatched == 1 && !net.counters.icmp_errors);
    error(3, 3); net_poll(&net, 3); check(net.event == NET_EVENT_ICMP_ERROR && net.counters.icmp_errors == 1);
    reset();
    for (i = 0; i < NET_ARP_ENTRIES + 1; ++i) { peer[3] = i + 1; send(); }
    peer[3] = 1;
    for (i = 0; i < NET_ARP_ENTRIES; ++i) if (net.arp[i].used) ++used;
    check(used == NET_ARP_ENTRIES && net.counters.arp_queries == NET_ARP_ENTRIES + 1);
    net_arp_invalidate(&net, 0); used = 0;
    for (i = 0; i < NET_ARP_ENTRIES; ++i) if (net.arp[i].used) ++used;
    check(!used);
    /* A send can observe link loss before the next INFO poll. */
    reset(); learn(); query(3000, 0); connected = 0;
    check(send() == NET_LINK_DOWN && !net.link && !net.pending);
    check(net_dns_poll(&dns, 2) == NET_DNS_LINK_DOWN && !dns.port);
    /* A fresh binding must not inherit errors from earlier unbound sends. */
    reset(); learn(); send(); callbacks = 0;
    check(net_udp_on_error(&net, 9000, receive_error) == NET_NO_PORT);
    net_udp_bind(&net, 9000, receive, 0); net_udp_on_error(&net, 9000, receive_error);
    error(3, 3); net_poll(&net, 2);
    check(!callbacks && net.counters.icmp_unmatched == 1 && !net.counters.icmp_errors);
}
int main(void) {
    unsigned short before;
    unsigned long epoch;
    reset();
    check(send() == NET_AGAIN && net.counters.arp_queries == 1);
    arp(2); check(net_poll(&net, 10) == NET_OK && send() == NET_OK && outgoing[5] == 2);
    arp(3); net_poll(&net, 20);
    check(send() == NET_OK && outgoing[5] == 3 && net.counters.arp_updates == 2);
    net_poll(&net, 60020UL);
    check(net.counters.arp_expired == 1 && send() == NET_AGAIN && read16(outgoing + 12) == 0x0806);
    arp(4); net_poll(&net, 60021UL); check(send() == NET_OK && outgoing[5] == 4);
    reset(); check(send() == NET_AGAIN);
    net_poll(&net, 999); check(send() == NET_AGAIN && net.counters.arp_queries == 1);
    net_poll(&net, 1000); send(); check(net.counters.arp_queries == 2);
    net_poll(&net, 2999); send(); check(net.counters.arp_queries == 2);
    net_poll(&net, 3000); send(); check(net.counters.arp_queries == 3);
    net_poll(&net, 6999); check(send() == NET_AGAIN);
    net_poll(&net, 7000); check(send() == NET_UNREACHABLE && net.counters.arp_failed == 1);
    net_poll(&net, 11999); check(send() == NET_UNREACHABLE);
    net_poll(&net, 12000); check(send() == NET_AGAIN && net.counters.arp_queries == 4);
    arp(5); net_poll(&net, 12001); check(send() == NET_OK && outgoing[5] == 5);
    reset(); block_tx = 1; check(send() == NET_AGAIN && net.pending);
    net_poll(&net, 9000); check(net.pending && !net.counters.arp_failed && !net.counters.arp_queries);
    block_tx = 0; net_poll(&net, 9001);
    check(send() == NET_AGAIN && net.counters.arp_queries == 1 && net.arp[0].requested == 9001UL);
    reset(); learn(); block_tx = 1;
    check(send() == NET_OK && net.pending && net.counters.tx_blocked == 1);
    net_arp_invalidate(&net, peer);
    check(!net.pending && net.counters.tx_discarded == 1 && !net.arp[0].used);
    block_tx = 0; check(send() == NET_AGAIN);
    reset(); learn(); block_tx = 1; query(3000, 0);
    check(dns.sent && net.pending);
    net_dns_cancel(&dns); before = transmissions; block_tx = 0; net_poll(&net, 2);
    check(!net.pending && !dns.port && net.counters.tx_discarded == 1 && transmissions == before);
    reset(); learn(); net_udp_bind(&net, 9999, receive, 0); block_tx = 1; query(3000, 0); epoch = net.link_epoch;
    connected = 0; check(net_poll(&net, 1001) == NET_LINK_DOWN);
    check(!net.pending && !net.arp[0].used && net.config.address[3] == 42 && net.link_epoch != epoch);
    check(net_dns_poll(&dns, 1001) == NET_DNS_LINK_DOWN && !dns.port && net.counters.link_down == 1);
    connected = 1; block_tx = 0; mac_tail = 3; net_poll(&net, 1251);
    check(net.link && net.mac[5] == 3 && net.counters.link_up == 1 && outgoing[11] == 3 && read16(outgoing + 12) == 0x0806);
    check(net_udp_bind(&net, 9999, receive, 0) == NET_NO_PORT);
    learn(); query(3000, 0); epoch = net.link_epoch; mac_tail = 4; net_poll(&net, 2251);
    check(net.link && net.link_epoch != epoch && !net.arp[0].used && net_dns_poll(&dns, 2251) == NET_DNS_LINK_DOWN);
    /* Detect RX link loss even before the periodic INFO refresh is due. */
    connected = 0; check(net_poll(&net, 2252) == NET_LINK_DOWN && !net.link);
    connected = 0; memset(&dhcp, 0, sizeof(dhcp));
    check(net_open(&net, 0, 7) == NET_OK && net_dhcp_start(&net, &dhcp, 0, 100) == NET_OK);
    check(net_dhcp_poll(&dhcp, 200000UL) == NET_LINK_DOWN && dhcp.state == NET_DHCP_WAIT_LINK && dhcp.active);
    connected = 1; net_dhcp_poll(&dhcp, 200250UL);
    check(dhcp.state == NET_DHCP_SELECTING && dhcp.started == 200250UL && !net.config.address[0]);
    net_dhcp_stop(&dhcp);
    /* Reject forged, truncated or unrelated quoted packets before DNS sees them. */
    reset(); learn(); query(100, 1); memcpy(old_quote, outgoing + 14, 28);
    error(3, 3); incoming[63] ^= 1; fix_error(); net_poll(&net, 2);
    check(dns.status == NET_DNS_WAITING && net.counters.icmp_unmatched == 1);
    error(3, 3); incoming[47] ^= 1; fix_quote(); net_poll(&net, 3);
    check(dns.status == NET_DNS_WAITING && net.counters.icmp_unmatched == 2);
    error(3, 3); incoming[54] = 10; fix_quote(); check(net_poll(&net, 4) == NET_REJECTED && dns.port);
    error(3, 3); incoming[52] ^= 1; fix_error(); check(net_poll(&net, 5) == NET_REJECTED && dns.port);
    error(3, 3); in_length = 69; put16(incoming + 16, 55); fix_error(); check(net_poll(&net, 6) == NET_REJECTED && dns.port);
    error(3, 3); incoming[36] ^= 1; check(net_poll(&net, 7) == NET_REJECTED && dns.port);
    error(3, 3); incoming[58] = 10; fix_quote(); net_poll(&net, 8);
    check(dns.status == NET_DNS_WAITING && net.counters.icmp_unmatched == 3);
    error(3, 3); put16(incoming + 48, 0x2000); fix_quote(); check(net_poll(&net, 9) == NET_REJECTED && dns.port);
    net_dns_poll(&dns, 101); error(3, 3); memcpy(incoming + 42, old_quote, 28); fix_error(); net_poll(&net, 102);
    check(dns.status == NET_DNS_WAITING && dns.attempts == 1 && net.counters.icmp_unmatched == 4);
    error(3, 3); incoming[50] = 1; fix_quote(); net_poll(&net, 103);
    check(net.event == NET_EVENT_ICMP_ERROR && dns.status == NET_DNS_UNREACHABLE && dns.icmp_code == 3 && !dns.port);
    error(3, 3); net_poll(&net, 104); check(net.counters.icmp_errors == 1 && net.counters.icmp_unmatched == 5);
    reset(); learn(); query(3000, 0); error(11, 0); net_poll(&net, 2);
    check(dns.status == NET_DNS_PATH_ERROR && dns.icmp_type == 11 && !dns.port);
    reset(); learn(); query(3000, 0); error(3, 4); net_poll(&net, 2);
    check(dns.status == NET_DNS_UNREACHABLE && dns.mtu == 1400);
    reset(); learn(); query(60000UL, 0); net_poll(&net, 30001UL); error(3, 3); net_poll(&net, 30002UL);
    check(dns.status == NET_DNS_WAITING && !net.counters.icmp_errors);
    net_dns_cancel(&dns);
    reset(); learn(); check(net_icmp_echo(&net, peer, 0x77, 5, &byte, 1) == NET_OK);
    error(12, 0); net_poll(&net, 2);
    check(net.event == NET_EVENT_ICMP_ERROR && net.error.protocol == 1 && net.error.identifier == 0x77 && net.error.sequence == 5);
    /* Wraparound, cache bounds, and counter saturation are explicit. */
    reset(); net.now = 0xfffffff0UL; learn(); net_poll(&net, 0x10000UL);
    check(!net.arp[0].ready && net.counters.arp_expired == 1);
    net.counters.tx_blocked = 0xffffffffUL; block_tx = 1; send();
    check(net.counters.tx_blocked == 0xffffffffUL);
    bounded_resources();
    putchar('\n'); return 0;
}
