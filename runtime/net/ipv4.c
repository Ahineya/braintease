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
    unsigned short header, total, local, boot;
    if (length < 34 || p[14] / 16 != 4) { ++net->rejected; return NET_REJECTED; }
    header = p[14] % 16 * 4; total = net_read16(p + 16);
    local = net->config.address[0] && !memcmp(p + 30, net->config.address, 4);
    boot = net->dhcp && p[23] == 17 && header >= 20 && total >= header + 8 &&
        total <= length - 14 && net_read16(p + 14 + header) == 67 && net_read16(p + 16 + header) == 68 &&
        ((!net->config.address[0]) || (p[30] == 255 && p[31] == 255 && p[32] == 255 && p[33] == 255));
    if (header < 20 || header > 60 || total < header || total > length - 14 ||
        (net_read16(p + 20) & 0xbfffU) || !p[22] || (!local && !boot) || net_checksum(p + 14, header)) {
        ++net->rejected; return NET_REJECTED;
    }
    if (p[23] == 1) return net_icmp_input(net, header, total - header);
    if (p[23] == 17) return net_udp_input(net, header, total - header);
    return NET_OK;
}
