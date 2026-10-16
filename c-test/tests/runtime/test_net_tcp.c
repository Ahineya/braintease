#include "net_tcp_test.h"
static void open_and_stream(void) {
    unsigned short count, before;
    reset(); check(net_tcp_connect(&net, &tcp, &target, 0) == NET_OK && tcp.state == NET_TCP_SYN_SENT);
    net_tcp_poll(&tcp, 0);
    check(outgoing[47] == 2 && read32(outgoing + 38) == 0xfffffff0UL && read16(outgoing + 56) == 512 && wire_ok());
    check(net_tcp_write(&tcp, data, 1, &count) == NET_AGAIN && !count);
    packet(18, peer_next - 1, tcp.snd_nxt + 1, 512, 0); tick(1); check(tcp.state == NET_TCP_SYN_SENT);
    packet(18, peer_next - 1, tcp.snd_nxt, 512, 0); incoming[50] ^= 1; tick(2); check(tcp.state == NET_TCP_SYN_SENT && net.rejected == 1);
    packet(18, peer_next - 1, tcp.snd_nxt, 512, 0); tick(10);
    check(tcp.state == NET_TCP_ESTABLISHED && tcp.mss == 128 && outgoing[47] == 16 && read32(outgoing + 42) == peer_next && wire_ok());
    check(net_tcp_write(&tcp, data, 600, &count) == NET_OK && count == 512);
    check(net_tcp_write(&tcp, data, 1, &count) == NET_AGAIN && !count);
    net_tcp_poll(&tcp, 11); check(tcp.flight_data == 128 && read16(outgoing + 16) == 168 && !memcmp(outgoing + 54, data, 128) && wire_ok());
    before = transmissions; net_tcp_poll(&tcp, 12); check(transmissions == before);
    packet(16, peer_next, tcp.snd_nxt + 1, 512, 0); tick(13); check(tcp.tx_used == 512 && tcp.bytes_acked == 0);
    packet(16, peer_next, tcp.snd_una + 64, 512, 0); tick(14);
    check(tcp.tx_used == 448 && tcp.flight_data == 64 && tcp.bytes_acked == 64 && tcp.snd_una == 0x31UL);
    packet(16, peer_next, tcp.snd_nxt, 512, 0); tick(15);
    check(tcp.tx_used == 384 && tcp.bytes_acked == 128 && tcp.flight_data == 128 && read32(outgoing + 38) == 0x71UL);
    packet(16, peer_next, tcp.snd_una - 1, 1, 0); tick(16); check(tcp.window == 512 && tcp.tx_used == 384);
    check(net_tcp_shutdown(&tcp) == NET_OK && net_tcp_write(&tcp, data, 1, &count) == NET_CLOSED);
    while (tcp.tx_used) { packet(16, peer_next, tcp.snd_nxt, 512, 0); tick(now + 1); }
    check(tcp.bytes_acked == 512 && tcp.state == NET_TCP_FIN_WAIT_1 && outgoing[47] == 17);
    packet(16, peer_next, tcp.snd_nxt, 512, 0); tick(now + 1); check(tcp.state == NET_TCP_FIN_WAIT_2);
    packet(17, peer_next, tcp.snd_nxt, 512, 0); tick(now + 1); ++peer_next;
    check(tcp.state == NET_TCP_TIME_WAIT && outgoing[47] == 16 && read32(outgoing + 42) == peer_next);
    packet(17, peer_next - 1, tcp.snd_nxt, 512, 0); tick(now + 1000);
    check(tcp.state == NET_TCP_TIME_WAIT && tcp.phase_at == now);
    tick(now + 119999UL); check(tcp.active);
    tick(now + 1); check(tcp.state == NET_TCP_CLOSED && !tcp.active && !net.tcp[0]);
    check(net_tcp_read(&tcp, data, 1, &count) == NET_EOF && !count);
}
static void resets_and_lifecycle(void) {
    unsigned short count;
    reset(); start(); packet(20, 77, tcp.snd_nxt + 1, 0, 0); tick(1); check(tcp.active);
    packet(20, 77, tcp.snd_nxt, 0, 0); tick(2); check(!tcp.active && tcp.error == NET_RESET);
    reset(); establish(); packet(4, peer_next + 1, 0, 0, 0); tick(11);
    check(tcp.active && outgoing[47] == 16);
    packet(4, peer_next + 1024, 0, 0, 0); tick(12); check(tcp.active);
    packet(4, peer_next, 0, 0, 0); tick(13); check(!tcp.active && net_tcp_read(&tcp, data, 1, &count) == NET_RESET);
    reset(); establish(); config.dns[3] = 2; net_configure(&net, &config);
    check(net_tcp_poll(&tcp, 20) == NET_CONFIG_CHANGED && !tcp.active); config.dns[3] = 1;
    reset(); establish(); connected = 0; tick(11);
    check(!tcp.active && tcp.error == NET_LINK_DOWN && !net.tcp[0]);
    reset(); establish(); blocked = 1; net_tcp_write(&tcp, data, 10, &count); net_tcp_poll(&tcp, 11);
    check(tcp.queued && net.pending && net.tcp_pending == &tcp);
    net_tcp_abort(&tcp); check(!tcp.active && !net.pending && !net.tcp_pending && tcp.error == NET_CANCELLED);
}
int main(void) { open_and_stream(); resets_and_lifecycle(); putchar('\n'); return 0; }
