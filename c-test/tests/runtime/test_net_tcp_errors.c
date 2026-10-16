#include "net_tcp_test.h"
static void error(unsigned short code) {
    memset(incoming, 0, sizeof(incoming)); memcpy(incoming, net.mac, 6); incoming[6] = 2; incoming[11] = 2;
    put16(incoming + 12, 0x0800); incoming[14] = 0x45; put16(incoming + 16, 56);
    incoming[22] = 64; incoming[23] = 1; memcpy(incoming + 26, target.peer, 4); memcpy(incoming + 30, config.address, 4);
    incoming[34] = 3; incoming[35] = code; if (code == 4) put16(incoming + 40, 128);
    memcpy(incoming + 42, outgoing + 14, 28); in_length = 70;
    put16(incoming + 24, fold(sum(incoming + 14, 20))); put16(incoming + 36, fold(sum(incoming + 34, 36)));
}
int main(void) {
    unsigned char old[28];
    reset(); start(); error(1); tick(1);
    check(tcp.active && tcp.state == NET_TCP_SYN_SENT && tcp.icmp_type == 3 && tcp.icmp_code == 1);
    reset(); start(); error(3); incoming[66] ^= 1; put16(incoming + 36, 0); put16(incoming + 36, fold(sum(incoming + 34, 36))); tick(1);
    check(tcp.active && net.counters.icmp_unmatched == 1);
    error(3); tick(2); check(tcp.error == NET_UNREACHABLE && !tcp.active && !net.tcp[0]);
    reset(); start(); error(4); tick(1); check(tcp.error == NET_PATH_ERROR && !tcp.active && tcp.mtu == 128);
    reset(); start(); memcpy(old, outgoing + 14, 28); net_tcp_abort(&tcp); net_tcp_connect(&net, &tcp, &target, 1); net_tcp_poll(&tcp, 1);
    error(3); memcpy(incoming + 42, old, 28); put16(incoming + 36, 0); put16(incoming + 36, fold(sum(incoming + 34, 36))); tick(2);
    check(tcp.active && net.counters.icmp_unmatched == 1);
    error(3); tick(3); check(tcp.error == NET_UNREACHABLE && !tcp.active);
    putchar('\n'); return 0;
}
