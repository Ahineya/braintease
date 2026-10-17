#include "net_tcp_test.h"
static NetTcpWaitTable waits;
static void finished(void) {
    establish(); net_tcp_shutdown(&tcp); tick(now + 1);
    packet(17, peer_next, tcp.snd_nxt, 512, 0); tick(now + 1);
}
int main(void) {
    unsigned short i, before;
    reset();
    check(net_tcp_retire(0) == NET_BAD_ARGUMENT);
    check(net_tcp_retire(&tcp) == NET_BAD_ARGUMENT);
    establish();
    check(net_tcp_retire(&tcp) == NET_BUSY && tcp.active && net.tcp[0] == &tcp);
    net_tcp_shutdown(&tcp); tick(now + 1);
    check(net_tcp_retire(&tcp) == NET_BUSY && tcp.active);
    packet(17, peer_next, tcp.snd_nxt, 512, 0); tick(now + 1);
    check(tcp.state == NET_TCP_TIME_WAIT);
    check(net_tcp_retire(&tcp) == NET_BUSY && tcp.active);
    check(net_tcp_wait_attach(&net, &waits) == NET_OK);
    check(net_tcp_retire(&tcp) == NET_OK && !tcp.active && !net.tcp[0] && waits.used == 1);
    check(net_tcp_retire(&tcp) == NET_OK && waits.used == 1);
    check(net_tcp_connect(&net, &tcp, &target, now) == NET_NO_PORT);
    before = transmissions;
    packet(17, peer_next, waits.entries[0].snd_nxt, 512, 0); net_poll(&net, now + 100);
    check(transmissions > before && outgoing[47] == 16 && wire_ok());
    check(waits.entries[0].used && waits.entries[0].started == now + 100);
    reset(); memset(&waits, 0, sizeof(waits)); net_tcp_wait_attach(&net, &waits); finished();
    for (i = 0; i < NET_TCP_WAIT_SLOTS; ++i) { waits.entries[i].used = 1; waits.entries[i].started = now; }
    waits.used = NET_TCP_WAIT_SLOTS;
    check(net_tcp_retire(&tcp) == NET_BUSY && tcp.active && waits.used == NET_TCP_WAIT_SLOTS);
    waits.entries[0].used = 0; --waits.used;
    check(net_tcp_retire(&tcp) == NET_OK && !tcp.active && waits.used == NET_TCP_WAIT_SLOTS);
    reset(); memset(&waits, 0, sizeof(waits)); net_tcp_wait_attach(&net, &waits); finished();
    ++net.generation;
    check(net_tcp_retire(&tcp) == NET_OK && !tcp.active && !waits.used);
    putchar('\n'); return 0;
}
