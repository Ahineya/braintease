/* Stream an exact binary pattern through guest TCP. The host is an ordinary
 * echo server; it never implements TCP on the guest's behalf. */
#include "demo.h"
#include <net_tcp.h>
#ifndef TCP_DHCP
#define TCP_DHCP 0
#endif
#if TCP_DHCP
#include <net_dhcp.h>
static NetDhcp lease;
#endif
#ifndef TCP_PORT
#define TCP_PORT 7777
#endif
#ifndef TCP_OCTETS
#define TCP_OCTETS 4097UL
#endif
#ifndef TCP_PEER_IP
#define TCP_PEER_IP 192,168,64,1
#endif
static NetContext network;
static NetTcp stream;
static NetTcpConfig remote;
static unsigned char sending[173], receiving[193];
static unsigned short octet(unsigned long index) { return (index * 37UL + 11UL) % 256UL; }
static int network_poll(unsigned long now_ms) {
#if TCP_DHCP
    return net_dhcp_poll(&lease, now_ms);
#else
    return net_poll(&network, now_ms);
#endif
}
static int initialize(VmClock *clock) {
#if TCP_DHCP
    if (net_open(&network, 0, 0x5246) || vm_get_time(clock) || net_dhcp_start(&network, &lease, clock->ms, 60000UL)) return 1;
    puts("TCP DHCP acquiring");
    while (lease.state != NET_DHCP_BOUND) {
        if (network_poll(clock->ms) == NET_IO_ERROR || lease.state == NET_DHCP_FAILED) return 1;
        if (vm_next_tick(clock)) return 1;
    }
#else
    if (net_init(&network, &config) || vm_get_time(clock)) return 1;
#endif
    { unsigned char address[4] = {TCP_PEER_IP}; memcpy(remote.peer, address, 4); }
    remote.remote_port = TCP_PORT; remote.timeout_ms = 60000UL;
#ifdef TCP_INITIAL_SEQUENCE
    remote.initial_sequence = TCP_INITIAL_SEQUENCE;
#else
    /* Local demonstration only: MAC + clock is not secret entropy. Production
     * applications must supply an unpredictable initial_sequence themselves. */
    remote.initial_sequence = clock->ms * 250UL + ((unsigned long)network.mac[4] * 256UL + network.mac[5]) * 65536UL;
#endif
    text("TCP local="); ip_text(network.config.address); text(" peer="); ip_text(remote.peer);
    text(" port="); number(remote.remote_port); text(" octets="); number(TCP_OCTETS); putchar('\n');
    return net_tcp_connect(&network, &stream, &remote, clock->ms);
}
int main(void) {
    VmClock clock;
    VmNetStats stats;
    unsigned long written = 0, read = 0;
    unsigned short i, size, count, eof = 0;
    int status;
    if (initialize(&clock)) return fail("TCP initialization failed", 1);
    while (stream.state != NET_TCP_CLOSED) {
        status = network_poll(clock.ms);
        if (status == NET_IO_ERROR) return fail("NIC polling failed", 2);
        status = net_tcp_poll(&stream, clock.ms);
        if (status != NET_OK && status != NET_AGAIN && status != NET_CLOSED) {
            text("TCP failed status="); number(status); text(" state="); number(stream.state); putchar('\n'); return 3;
        }
        if ((stream.state == NET_TCP_ESTABLISHED || stream.state == NET_TCP_CLOSE_WAIT) && written < TCP_OCTETS) {
            size = TCP_OCTETS - written < sizeof(sending) ? TCP_OCTETS - written : sizeof(sending);
            for (i = 0; i < size; ++i) sending[i] = octet(written + i);
            status = net_tcp_write(&stream, sending, size, &count);
            if (status != NET_OK && status != NET_AGAIN) return fail("TCP write failed", 4);
            written += count;
        }
        if (!eof) {
            status = net_tcp_read(&stream, receiving, sizeof(receiving), &count);
            if (status != NET_OK && status != NET_AGAIN && status != NET_EOF) return fail("TCP read failed", 5);
            if (read + count > TCP_OCTETS) return fail("extra echo bytes", 6);
            for (i = 0; i < count; ++i) if (receiving[i] != octet(read + i)) return fail("echo data mismatch", 7);
            read += count;
            if (status == NET_EOF) {
                if (read != TCP_OCTETS || written != TCP_OCTETS) return fail("early echo EOF", 8);
                eof = 1;
            }
        }
        if (eof && !stream.shutting && stream.state != NET_TCP_CLOSED && net_tcp_shutdown(&stream)) return fail("TCP shutdown failed", 9);
        if (vm_next_tick(&clock)) return fail("clock tick failed", 10);
    }
    if (!eof || stream.bytes_acked != TCP_OCTETS) return fail("incomplete TCP exchange", 11);
    do {
        if (network_poll(clock.ms) == NET_IO_ERROR || vm_net_stats(0, &stats) || stats.tx_failed || stats.backend_errors) return fail("TCP TX drain failed", 12);
        if (vm_next_tick(&clock)) return fail("clock tick failed", 13);
    } while (network.pending || stats.tx_sent != stats.tx_accepted);
    text("TCP OK written="); number(written); text(" acknowledged="); number(stream.bytes_acked); text(" received="); number(read);
    text(" retransmits="); number(stream.retransmits); text(" probes="); number(stream.probes); putchar('\n');
    counters(&stats); return 0;
}
