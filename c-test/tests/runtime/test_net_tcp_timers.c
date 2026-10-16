#include "net_tcp_test.h"
static NetTcp more[4];
static void retransmission(void) {
    unsigned short count, before, i;
    unsigned long end, time;
    reset(); start(); before = transmissions;
    tick(999); check(transmissions == before);
    tick(1000); check(transmissions == before + 1 && tcp.retransmits == 1 && tcp.rto == 2000 && read32(outgoing + 38) == target.initial_sequence);
    packet(18, peer_next - 1, tcp.snd_nxt, 512, 0); tick(1010);
    check(tcp.state == NET_TCP_ESTABLISHED && tcp.base_rto >= 3000UL);
    reset(); start(); net_arp_invalidate(&net, 0); tick(1000);
    check(!tcp.flight_sent && tcp.sent_nxt == tcp.snd_nxt);
    packet(18, peer_next - 1, tcp.snd_nxt, 512, 0); tick(1001);
    check(tcp.state == NET_TCP_ESTABLISHED);
    reset(); establish(); net_tcp_write(&tcp, data, 128, &count); net_tcp_poll(&tcp, 11); end = tcp.snd_nxt;
    tick(1011); check(tcp.retransmits == 1 && tcp.snd_nxt == end && tcp.rto == 2000 && wire_ok());
    packet(16, peer_next, end, 512, 0); tick(1012); check(tcp.bytes_acked == 128 && !tcp.tx_used && !tcp.inflight);
    reset(); blocked = 1; start(); tick(10000);
    check(tcp.queued && !tcp.flight_sent && !tcp.retransmits && !tcp.retries);
    blocked = 0; tick(10001); before = transmissions;
    check(tcp.flight_sent && tcp.sent_at == 10001UL);
    tick(11000); check(transmissions == before);
    tick(11001); check(tcp.retransmits == 1);
    reset(); now = 0xfffffff0UL; route(); start(); before = transmissions;
    tick(0x3d7UL); check(transmissions == before);
    tick(0x3d8UL); check(tcp.retransmits == 1);
    reset(); target.timeout_ms = 25; start(); tick(24); check(tcp.active);
    tick(25); check(tcp.error == NET_TIMEOUT && !tcp.active && !net.tcp[0]); target.timeout_ms = 60000;
    reset(); target.timeout_ms = 600000UL; start(); time = 0;
    for (i = 0; i < 8; ++i) { time += tcp.rto; now = time; route(); tick(time); }
    check(!tcp.active && tcp.error == NET_TIMEOUT && tcp.retransmits == 7); target.timeout_ms = 60000;
}
static void windows_and_pending(void) {
    unsigned short count;
    unsigned long first;
    reset(); establish(); packet(16, peer_next, tcp.snd_una, 0, 0); tick(11);
    check(tcp.window == 0); net_tcp_write(&tcp, data, 10, &count); net_tcp_poll(&tcp, 12); first = tcp.snd_una;
    check(tcp.flight_data == 1 && read16(outgoing + 16) == 41 && tcp.probes == 1);
    tick(1012); check(tcp.probes == 2 && tcp.retransmits == 1 && tcp.snd_una == first);
    packet(16, peer_next, first, 5, 0); tick(1013);
    check(tcp.window == 5 && read32(outgoing + 38) == first && read16(outgoing + 16) == 41);
    packet(16, peer_next, first + 1, 5, 0); tick(1014);
    check(tcp.flight_data == 5 && tcp.bytes_acked == 1 && read16(outgoing + 16) == 45);
    packet(16, peer_next, tcp.snd_nxt, 5, 0); tick(1015); check(tcp.flight_data == 4 && tcp.bytes_acked == 6);
    packet(16, peer_next, tcp.snd_nxt, 5, 0); tick(1016); check(tcp.bytes_acked == 10 && !tcp.tx_used);
    reset(); establish(); packet(16, peer_next, tcp.snd_una, 0, 0); tick(11);
    net_tcp_shutdown(&tcp); net_tcp_poll(&tcp, 12);
    check(tcp.state == NET_TCP_ESTABLISHED && outgoing[47] == 16 && !tcp.inflight && tcp.probes == 1);
    packet(16, peer_next, tcp.snd_una, 1, 0); tick(13); check(tcp.state == NET_TCP_FIN_WAIT_1 && outgoing[47] == 17);
    reset(); establish(); net_tcp_write(&tcp, data, 10, &count); blocked = 1; net_tcp_poll(&tcp, 11);
    check(net.pending && tcp.queued); net_arp_invalidate(&net, target.peer);
    check(!net.pending && !tcp.queued && tcp.active);
    blocked = 0; now = 12; route(); net_tcp_poll(&tcp, now);
    check(tcp.flight_sent && tcp.bytes_written == 10 && tcp.snd_nxt - tcp.snd_una == 10);
    /* A planned segment awaiting ARP must not be acknowledged as transmitted. */
    reset(); establish(); net_arp_invalidate(&net, 0); net_tcp_write(&tcp, data, 10, &count); net_tcp_poll(&tcp, 11);
    check(!tcp.flight_sent && tcp.sent_nxt == tcp.snd_una);
    packet(16, peer_next, tcp.snd_nxt, 512, 0); tick(12); check(tcp.tx_used == 10 && !tcp.bytes_acked);
}
static void slots(void) {
    unsigned short i;
    reset(); memset(more, 0, sizeof(more)); start(); target.local_port = 0;
    for (i = 0; i < 3; ++i) check(net_tcp_connect(&net, &more[i], &target, 0) == NET_OK);
    check(net_tcp_connect(&net, &more[3], &target, 0) == NET_NO_PORT && !more[3].active);
    net_tcp_abort(&tcp); target.local_port = 50000;
    check(net_tcp_connect(&net, &more[3], &target, 0) == NET_OK);
    memset(&tcp, 0, sizeof(tcp)); check(net_tcp_connect(&net, &tcp, &target, 0) == NET_NO_PORT);
    for (i = 0; i < 4; ++i) net_tcp_abort(&more[i]);
    check(net_tcp_connect(&net, &tcp, &target, 0) == NET_OK); net_tcp_abort(&tcp);
}
int main(void) { retransmission(); windows_and_pending(); slots(); putchar('\n'); return 0; }
