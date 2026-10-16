#include "internal.h"

unsigned short net_read16(const unsigned char *p) { return p[0] * 256U + p[1]; }
void net_write16(unsigned char *p, unsigned short v) { p[0] = v / 256U; p[1] = v % 256U; }
unsigned short net_sum(unsigned short sum, const unsigned char *p, unsigned short length) {
    unsigned short before, word;
    while (length >= 2) {
        word = net_read16(p); before = sum; sum += word;
        if (sum < before) ++sum;
        p += 2; length -= 2;
    }
    if (length) { before = sum; sum += p[0] * 256U; if (sum < before) ++sum; }
    return sum;
}
unsigned short net_checksum(const unsigned char *p, unsigned short length) {
    return 65535U - net_sum(0, p, length);
}
unsigned short net_udp_checksum(const unsigned char *source, const unsigned char *destination,
                                 const unsigned char *udp, unsigned short length) {
    return net_l4_checksum(source, destination, 17, udp, length);
}
unsigned short net_l4_checksum(const unsigned char *source, const unsigned char *destination,
                              unsigned short protocol, const unsigned char *data, unsigned short length) {
    unsigned char pseudo[12];
    memcpy(pseudo, source, 4); memcpy(pseudo + 4, destination, 4);
    pseudo[8] = 0; pseudo[9] = protocol; net_write16(pseudo + 10, length);
    return 65535U - net_sum(net_sum(0, pseudo, 12), data, length);
}
unsigned short net_random(NetContext *net) {
    net->random = net->random * 25173U + 13849U;
    return net->random;
}
void net_ethernet(NetContext *net, const unsigned char *destination, unsigned short type) {
    unsigned short i;
    for (i = 0; i < 6; ++i) net->tx[i] = destination ? destination[i] : 255;
    memcpy(net->tx + 6, net->mac, 6); net_write16(net->tx + 12, type);
}
int net_transmit(NetContext *net, unsigned short length) {
    unsigned short outcome;
    if (!net->link) return NET_LINK_DOWN;
    while (length < 60) net->tx[length++] = 0;
    if (vm_net_tx(net->config.interface, net->tx, length, &outcome)) return NET_IO_ERROR;
    if (outcome == VM_NET_WOULD_BLOCK) { net_count(&net->counters.tx_blocked); net->pending = length; return NET_OK; }
    if (outcome == VM_NET_LINK_DOWN) { net_link_lost(net); return NET_LINK_DOWN; }
    if (outcome != VM_NET_OK) return NET_IO_ERROR;
    net->pending = 0;
    net_arp_sent(net);
    net_remember_packet(net, length);
    if (net->tcp_driver && net->tcp_pending) net->tcp_driver->transmitted(net);
    return NET_OK;
}
