#include "net_tcp_test.h"
static unsigned char result[600];
static void ordered_stream(void) {
    unsigned short count;
    unsigned long first;
    reset(); establish(); first = peer_next;
    packet(24, first + 3, tcp.snd_nxt, 512, 3); tick(11);
    check(!tcp.rx_used && tcp.rcv_nxt == first && read32(outgoing + 42) == first);
    packet(24, first, tcp.snd_nxt, 512, 5); tick(12);
    check(tcp.rx_used == 5 && tcp.rcv_nxt == first + 5 && tcp.bytes_received == 5);
    check(net_tcp_read(&tcp, result, 2, &count) == NET_OK && count == 2 && result[0] == 0 && result[1] == 1);
    packet(24, first, tcp.snd_nxt, 512, 5); tick(13); check(tcp.rx_used == 3 && tcp.bytes_received == 5);
    packet(24, first + 3, tcp.snd_nxt, 512, 5); tick(14);
    check(tcp.rx_used == 6 && tcp.rcv_nxt == 0 && tcp.bytes_received == 8);
    check(net_tcp_read(&tcp, result, 600, &count) == NET_OK && count == 6 && result[0] == 2 && result[2] == 4 && result[3] == 2 && result[5] == 4);
    check(net_tcp_read(&tcp, result, 1, &count) == NET_AGAIN && !count);
    packet(24, tcp.rcv_nxt, tcp.snd_nxt, 512, 600); tick(15);
    check(tcp.rx_used == 512 && tcp.rcv_nxt == 512 && read16(outgoing + 48) == 0 && tcp.bytes_received == 520);
    packet(17, tcp.rcv_nxt, tcp.snd_nxt, 512, 0); tick(16); check(!tcp.peer_fin && tcp.state == NET_TCP_ESTABLISHED);
    check(net_tcp_read(&tcp, result, 200, &count) == NET_OK && count == 200);
    net_tcp_poll(&tcp, 17); check(read16(outgoing + 48) == 200 && outgoing[47] == 16);
    packet(25, tcp.rcv_nxt, tcp.snd_nxt, 512, 88); tick(18);
    check(tcp.peer_fin && tcp.state == NET_TCP_CLOSE_WAIT && tcp.rx_used == 400 && tcp.rcv_nxt == 601);
    check(net_tcp_read(&tcp, result, 600, &count) == NET_OK && count == 400);
    check(net_tcp_read(&tcp, result, 600, &count) == NET_EOF && !count);
}
static void half_close(void) {
    unsigned short count;
    reset(); establish(); packet(17, peer_next, tcp.snd_nxt, 512, 0); tick(11); ++peer_next;
    check(tcp.state == NET_TCP_CLOSE_WAIT && net_tcp_read(&tcp, result, 1, &count) == NET_EOF);
    check(net_tcp_write(&tcp, data, 3, &count) == NET_OK && count == 3); net_tcp_poll(&tcp, 12);
    packet(16, peer_next, tcp.snd_nxt, 512, 0); tick(13); check(tcp.bytes_acked == 3 && !tcp.inflight);
    net_tcp_shutdown(&tcp); net_tcp_poll(&tcp, 14); check(tcp.state == NET_TCP_LAST_ACK && outgoing[47] == 17);
    packet(16, peer_next, tcp.snd_nxt, 512, 0); tick(15);
    check(tcp.state == NET_TCP_CLOSED && !tcp.active && !net.tcp[0]);
    check(net_tcp_read(&tcp, result, 1, &count) == NET_EOF);
    target.local_port = 0; check(net_tcp_connect(&net, &tcp, &target, 16) == NET_OK); net_tcp_abort(&tcp); target.local_port = 50000;
    reset(); establish(); net_tcp_shutdown(&tcp); net_tcp_poll(&tcp, 11);
    packet(17, peer_next, tcp.snd_una, 512, 0); tick(12); ++peer_next;
    check(tcp.state == NET_TCP_CLOSING && tcp.peer_fin);
    packet(16, peer_next, tcp.snd_nxt, 512, 0); tick(13); check(tcp.state == NET_TCP_TIME_WAIT);
    net_tcp_abort(&tcp);
}
static void malformed_and_closed(void) {
    unsigned short before;
    reset(); start();
    packet(18, peer_next - 1, tcp.snd_nxt, 512, 0); incoming[55] = 1; fix_packet(); tick(1);
    check(tcp.state == NET_TCP_SYN_SENT && net.rejected == 1);
    packet(18, peer_next - 1, tcp.snd_nxt, 512, 0); put16(incoming + 56, 0); fix_packet(); tick(2);
    check(tcp.state == NET_TCP_SYN_SENT && net.rejected == 2);
    packet(18, peer_next - 1, tcp.snd_nxt, 512, 0); tick(10);
    packet(16, peer_next, tcp.snd_nxt, 512, 0); incoming[46] = 240; fix_packet(); tick(11); check(net.rejected == 3 && tcp.active);
    packet(16, peer_next, tcp.snd_nxt, 512, 0); put16(incoming + 36, 0); fix_packet(); tick(12); check(net.rejected == 4 && tcp.active);
    packet(16, peer_next, tcp.snd_nxt, 512, 0); put16(incoming + 20, 0x2000); fix_packet(); tick(13); check(net.rejected == 5 && tcp.active);
    reset(); net_tcp_enable(&net); packet(2, 99, 0, 512, 0); put16(incoming + 36, 81); fix_packet(); net_poll(&net, 0);
    check(outgoing[47] == 20 && read32(outgoing + 38) == 0 && read32(outgoing + 42) == 100 && wire_ok());
    before = transmissions; packet(4, 100, 0, 0, 0); put16(incoming + 36, 81); fix_packet(); net_poll(&net, 1);
    check(transmissions == before);
    packet(16, 100, 123, 512, 0); put16(incoming + 36, 81); fix_packet(); net_poll(&net, 2); check(transmissions == before);
    packet(16, 100, 123, 512, 0); put16(incoming + 36, 81); fix_packet(); net_poll(&net, 100);
    check(outgoing[47] == 4 && read32(outgoing + 38) == 123 && wire_ok());
    reset(); establish(); packet(48, peer_next, tcp.snd_nxt, 512, 0); tick(11);
    check(!tcp.active && tcp.error == NET_UNSUPPORTED);
}
int main(void) { ordered_stream(); half_close(); malformed_and_closed(); putchar('\n'); return 0; }
