/* Deterministic fake NIC and clock: exercise real guest UDP/DHCP/ARP paths. */
#include <net_dhcp.h>
#include <net_dns.h>
#include <stdio.h>
#include <string.h>
static NetContext net;
static NetDhcp lease;
static NetDns lookup;
static unsigned char incoming[600], outgoing[600];
static unsigned short in_length, out_length, transmissions, block_tx, connected = 1;
static unsigned char address[4] = {192,168,64,42};
static unsigned char server[4] = {192,168,64,1};
static unsigned long now;
unsigned short vm_net_info(unsigned short id, VmNetInfo *info) {
    memset(info, 0, sizeof(*info)); info->link = connected; info->mtu = 1500; info->max_frame = 1514;
    info->mac[0] = 2; info->mac[5] = 1; return 0;
}
unsigned short vm_net_tx(unsigned short id, const unsigned char *p, unsigned short length, unsigned short *outcome) {
    if (!connected) { *outcome = VM_NET_LINK_DOWN; return 0; }
    if (block_tx) { *outcome = VM_NET_WOULD_BLOCK; return 0; }
    *outcome = VM_NET_OK; out_length = length; ++transmissions; memcpy(outgoing, p, length); return 0;
}
unsigned short vm_net_rx(unsigned short id, unsigned char *p, unsigned short cap, VmNetReceive *result) {
    if (!connected) { result->outcome = VM_NET_LINK_DOWN; result->length = 0; return 0; }
    result->outcome = in_length ? VM_NET_OK : VM_NET_EMPTY; result->length = in_length;
    memcpy(p, incoming, in_length); in_length = 0; return 0;
}
static void check(int ok) { putchar(ok ? 'Y' : 'N'); }
static unsigned short read16(const unsigned char *p) { return p[0] * 256U + p[1]; }
static void put16(unsigned char *p, unsigned short v) { p[0] = v / 256U; p[1] = v % 256U; }
static void put32(unsigned char *p, unsigned long v) { put16(p, v / 65536UL); put16(p + 2, v % 65536UL); }
static unsigned short checksum(unsigned char *p, unsigned short n) {
    unsigned long sum = 0;
    while (n >= 2) { sum += read16(p); p += 2; n -= 2; }
    if (n) sum += p[0] * 256UL;
    while (sum > 65535UL) sum = (sum & 65535UL) + sum / 65536UL;
    return 65535UL - sum;
}
static unsigned short option(unsigned short code) {
    unsigned short i = 282, size;
    while (i < out_length && outgoing[i] != 255) {
        if (!outgoing[i]) { ++i; continue; }
        size = outgoing[i + 1]; if (outgoing[i] == code) return i + 2; i += size + 2;
    }
    return 0;
}
static void tick(unsigned long time) { now = time; net_dhcp_poll(&lease, now); }
static void reply(unsigned short type, unsigned long duration, unsigned short other_server) {
    unsigned char *p = incoming + 42;
    unsigned short i = 240;
    memset(incoming, 0, sizeof(incoming)); incoming[0] = 2; incoming[5] = 1; incoming[6] = 2; incoming[11] = 2;
    incoming[12] = 8; incoming[14] = 0x45; incoming[22] = 64; incoming[23] = 17;
    memcpy(incoming + 26, server, 4); memset(incoming + 30, 255, 4);
    put16(incoming + 34, 67); put16(incoming + 36, 68);
    p[0] = 2; p[1] = 1; p[2] = 6; put32(p + 4, lease.xid); memcpy(p + 16, address, 4); memcpy(p + 28, net.mac, 6);
    p[236] = 99; p[237] = 130; p[238] = 83; p[239] = 99;
    p[i++] = 53; p[i++] = 1; p[i++] = type;
    p[i++] = 54; p[i++] = 4; memcpy(p + i, server, 4); if (other_server) p[i + 3] = 2; i += 4;
    p[i++] = 51; p[i++] = 4; put32(p + i, duration); i += 4;
    p[i++] = 1; p[i++] = 4; p[i++] = 255; p[i++] = 255; p[i++] = 255; p[i++] = 0;
    p[i++] = 3; p[i++] = 4; memcpy(p + i, server, 4); i += 4;
    p[i++] = 6; p[i++] = 4; p[i++] = 1; p[i++] = other_server ? 0 : 1; p[i++] = other_server ? 0 : 1; p[i++] = 1;
    p[i++] = 255;
    in_length = i + 42; put16(incoming + 38, i + 8); put16(incoming + 16, i + 28);
    put16(incoming + 24, checksum(incoming + 14, 20));
}
static void bind_lease(unsigned long duration) {
    reply(2, duration, 0); tick(now + 1);
    reply(5, duration, 0); tick(now + 1);
    tick(now + 1001); tick(now + 2001); tick(now + 2001); tick(now + 2001);
}
static void route(void) {
    net.arp[0].used = 1; net.arp[0].ready = 1;
    net.arp[0].resolved = now;
    memcpy(net.arp[0].address, server, 4); net.arp[0].mac[0] = 2; net.arp[0].mac[5] = 2;
}
static void conflict(void) {
    memset(incoming, 0, 60); memset(incoming, 255, 6); incoming[6] = 2; incoming[11] = 2;
    incoming[12] = 8; incoming[13] = 6; put16(incoming + 14, 1); put16(incoming + 16, 0x0800);
    incoming[18] = 6; incoming[19] = 4; put16(incoming + 20, 1); incoming[22] = 2; incoming[27] = 2;
    memcpy(incoming + 28, address, 4); memcpy(incoming + 38, server, 4); in_length = 60;
}
static void terminal_conflicts(void) {
    unsigned short before;
    /* A terminal conflict must still transmit DECLINE after local backpressure. */
    check(net_dhcp_start(&net, &lease, 0, 60000UL) == NET_OK);
    tick(10001); reply(2, 3600, 0); tick(now + 1); reply(5, 3600, 0); tick(now + 1);
    lease.conflicts = 2; block_tx = 1; conflict(); tick(now + 1);
    check(lease.state == NET_DHCP_BACKOFF && lease.active && net.pending && !net.config.address[0]);
    tick(now + 1); check(lease.active && net.pending);
    block_tx = 0; tick(now + 1);
    check(lease.state == NET_DHCP_FAILED && lease.error == NET_REJECTED && !net.dhcp && !net.pending && outgoing[284] == 4);
    /* A bound client must not answer ARP using an address it just relinquished. */
    check(net_dhcp_start(&net, &lease, 0, 60000UL) == NET_OK);
    tick(10001); bind_lease(3600UL); check(lease.state == NET_DHCP_BOUND);
    before = transmissions; conflict(); memcpy(incoming + 38, address, 4); tick(now + 1);
    check(lease.state == NET_DHCP_BACKOFF && !net.config.address[0] && transmissions == before + 1 && outgoing[284] == 4);
}
static void link_lifecycle(void) {
    /* Link loss removes a bound lease; restore starts fresh acquisition. */
    net_dhcp_stop(&lease);
    check(net_dhcp_start(&net, &lease, now, 60000UL) == NET_OK);
    tick(now + 10001); bind_lease(3600UL); check(lease.state == NET_DHCP_BOUND);
    connected = 0; tick(now + 1000);
    check(lease.state == NET_DHCP_WAIT_LINK && lease.active && !net.config.address[0]);
    tick(now + 100000UL); check(lease.active && lease.state == NET_DHCP_WAIT_LINK);
    connected = 1; tick(now + 250);
    check(lease.state == NET_DHCP_SELECTING && !net.config.address[0]);
    tick(now + 10001); bind_lease(3600UL);
    check(lease.state == NET_DHCP_BOUND && net.config.address[0] && lease.acquired == 2);
}
int main(void) {
    unsigned long origin, old_xid, generation;
    unsigned short before;
    check(net_open(&net, 0, 7) == NET_OK && !net.config.address[0] && !transmissions);
    check(net_dhcp_start(&net, &lease, 0, 60000UL) == NET_OK && net.dhcp);
    tick(10001);
    check(outgoing[282] == 53 && outgoing[284] == 1 && outgoing[26] == 0 && outgoing[30] == 255 && read16(outgoing + 34) == 68);
    old_xid = lease.xid;
    reply(2, 3600, 0); incoming[46] ^= 1; tick(now + 1);
    check(lease.state == NET_DHCP_SELECTING);
    reply(2, 3600, 0); incoming[70] ^= 1; tick(now + 1);
    check(lease.state == NET_DHCP_SELECTING);
    reply(2, 3600, 0); incoming[278] = 0; tick(now + 1);
    check(lease.malformed == 1 && lease.state == NET_DHCP_SELECTING);
    reply(2, 3600, 0); incoming[283] = 255; tick(now + 1);
    check(lease.malformed == 2 && lease.state == NET_DHCP_SELECTING);
    reply(2, 3600, 0); tick(now + 1);
    check(lease.state == NET_DHCP_REQUESTING && outgoing[284] == 3 && option(50) && option(54) && !memcmp(outgoing + option(50), address, 4));
    reply(5, 3600, 1); tick(now + 1);
    check(lease.state == NET_DHCP_REQUESTING);
    reply(5, 3600, 0); tick(now + 1);
    check(lease.state == NET_DHCP_PROBING && !net.config.address[0]);
    tick(now + 1001); check(lease.probes == 1 && outgoing[28] == 0 && !memcmp(outgoing + 38, address, 4));
    tick(now + 2001); tick(now + 2001);
    check(lease.probes == 3 && !net.config.address[0]);
    tick(now + 2001);
    check(lease.state == NET_DHCP_BOUND && !memcmp(net.config.address, address, 4) && lease.acquired == 1 && lease.t1 == 1800 && lease.t2 == 3150);
    generation = net.generation;
    check(net_dns_start(&net, &lookup, "example.org", now, 60000, 0) == NET_OK);
    origin = lease.request_at; route(); tick(origin + 1800000UL);
    check(lease.state == NET_DHCP_RENEWING && read16(outgoing + 12) == 0x0806);
    route(); tick(now + 1);
    check(lease.state == NET_DHCP_RENEWING && lease.xid != old_xid && outgoing[30] == 192 && outgoing[54] == 192 && !option(50) && !option(54));
    before = transmissions; tick(now + 60000UL);
    check(transmissions == before);
    tick(origin + 3150000UL);
    check(lease.state == NET_DHCP_REBINDING && outgoing[30] == 255 && outgoing[54] == 192 && !option(54));
    reply(5, 7200, 1); tick(now + 1);
    check(lease.state == NET_DHCP_BOUND && lease.renewals == 1 && net.config.dns[1] == 0 && net.generation != generation);
    check(net_dns_poll(&lookup, now) == NET_DNS_CONFIG_CHANGED && !lookup.port);
    origin = lease.request_at; tick(origin + 7200000UL);
    check(lease.expirations == 1 && !net.config.address[0] && lease.state == NET_DHCP_SELECTING && !net.arp[0].used);
    tick(now + 10001); bind_lease(0xffffffffUL);
    check(lease.state == NET_DHCP_BOUND && lease.lease == 0xffffffffUL);
    tick(now + 4000000000UL);
    check(lease.state == NET_DHCP_BOUND && net.config.address[0]);
    net_dhcp_stop(&lease);
    check(!net.dhcp && !net.config.address[0] && lease.state == NET_DHCP_STOPPED);
    check(net_dhcp_start(&net, &lease, 0xfffffff0UL, 20000UL) == NET_OK);
    tick(0x2710UL); check(lease.state == NET_DHCP_SELECTING && outgoing[284] == 1);
    tick(0x4e20UL); check(lease.state == NET_DHCP_FAILED && lease.error == NET_TIMEOUT && !net.dhcp);
    check(net_dhcp_start(&net, &lease, 0, 60000UL) == NET_OK);
    tick(10001); reply(2, 3600, 0); tick(now + 1); reply(5, 3600, 0); tick(now + 1);
    conflict(); tick(now + 1);
    check(lease.state == NET_DHCP_BACKOFF && lease.conflicts == 1 && !net.config.address[0] && outgoing[284] == 4 && option(50));
    tick(now + 9999); check(lease.state == NET_DHCP_BACKOFF);
    tick(now + 1); check(lease.state == NET_DHCP_SELECTING);
    net_dhcp_stop(&lease);
    /* NAK returns to acquisition without retaining the rejected address. */
    check(net_dhcp_start(&net, &lease, 0, 60000UL) == NET_OK);
    tick(10001); reply(2, 3600, 0); tick(now + 1); reply(6, 3600, 0); tick(now + 1);
    check(lease.state == NET_DHCP_BACKOFF && !net.config.address[0] && lease.error == NET_UNREACHABLE);
    tick(now + 1001); check(lease.state == NET_DHCP_SELECTING);
    tick(60001); check(lease.state == NET_DHCP_FAILED && lease.error == NET_TIMEOUT);
    /* Option overload in the BOOTP file field; require bounded termination. */
    check(net_dhcp_start(&net, &lease, 0, 60000UL) == NET_OK);
    tick(10001); reply(2, 3600, 0);
    incoming[42 + 267] = 99; /* old DNS option becomes an unknown option */
    incoming[42 + 273] = 52; incoming[42 + 274] = 1; incoming[42 + 275] = 1; incoming[42 + 276] = 255;
    incoming[42 + 108] = 6; incoming[42 + 109] = 4;
    memset(incoming + 42 + 110, 9, 4); incoming[42 + 114] = 255;
    in_length = 42 + 277; put16(incoming + 38, 285); put16(incoming + 16, 305);
    put16(incoming + 24, 0); put16(incoming + 24, checksum(incoming + 14, 20));
    tick(now + 1); check(lease.state == NET_DHCP_REQUESTING && lease.offered.dns[0] == 9);
    reply(5, 3600, 0); incoming[42 + 249] = 99; tick(now + 1);
    check(lease.state == NET_DHCP_REQUESTING); /* missing lease */
    reply(5, 3600, 0); memset(incoming + 42 + 16, 0, 4); tick(now + 1);
    check(lease.state == NET_DHCP_REQUESTING); /* missing assigned address */
    reply(5, 10000000UL, 0); tick(now + 1);
    tick(now + 1001); tick(now + 2001); tick(now + 2001); tick(now + 2001);
    tick(now + 2000000000UL); tick(now + 2000000000UL); tick(now + 2000000000UL);
    check(lease.age >= 6000000UL && lease.expirations == 0 && net.config.address[0]);
    tick(now + 4000000000UL); check(lease.expirations == 1 && !net.config.address[0]);
    /* Explicit reconfiguration relinquishes DHCP ownership, preserving the caller's IP. */
    lease.offered.address[0] = 192; lease.offered.address[1] = 168; lease.offered.address[2] = 64; lease.offered.address[3] = 77;
    lease.offered.mask[0] = 255; lease.offered.mask[1] = 255; lease.offered.mask[2] = 255; lease.offered.mask[3] = 0;
    check(net_configure(&net, &lease.offered) == NET_OK);
    check(net_dhcp_poll(&lease, now + 1) == NET_CONFIG_CHANGED && !net.dhcp && net.config.address[3] == 77);
    terminal_conflicts();
    link_lifecycle();
    putchar('\n'); return 0;
}
