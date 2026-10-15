#include "internal.h"

void net_ipv4_header(NetContext *net, const unsigned char *destination, unsigned short protocol, unsigned short length) {
    unsigned char *p = net->tx + 14;
    memset(p, 0, 20); p[0] = 0x45; net_write16(p + 2, length + 20);
    net_write16(p + 4, ++net->packet_id); p[8] = 64; p[9] = protocol;
    memcpy(p + 12, net->config.address, 4); memcpy(p + 16, destination, 4);
    net_write16(p + 10, net_checksum(p, 20));
}
int net_ipv4_input(NetContext *net, unsigned short length) {
    unsigned char *p = net->rx;
    unsigned short header, total;
    if (length < 34 || p[14] / 16 != 4) { ++net->rejected; return NET_REJECTED; }
    header = p[14] % 16 * 4; total = net_read16(p + 16);
    if (header < 20 || header > 60 || total < header || total > length - 14 ||
        (net_read16(p + 20) & 0xbfffU) || !p[22] || memcmp(p + 30, net->config.address, 4) || net_checksum(p + 14, header)) {
        ++net->rejected; return NET_REJECTED;
    }
    if (p[23] == 1) return net_icmp_input(net, header, total - header);
    if (p[23] == 17) return net_udp_input(net, header, total - header);
    return NET_OK;
}
