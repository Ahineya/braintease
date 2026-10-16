#include "dhcp_internal.h"

unsigned long dhcp_read32(const unsigned char *p) {
    return (unsigned long)net_read16(p) * 65536UL + net_read16(p + 2);
}
void dhcp_write32(unsigned char *p, unsigned long value) {
    net_write16(p, value / 65536UL);
    net_write16(p + 2, value % 65536UL);
}
int dhcp_ip(const unsigned char *p) {
    return p[0] && p[0] < 224 && p[0] != 127;
}
int dhcp_send(NetDhcp *dhcp, unsigned short type) {
    NetContext *net = dhcp->net;
    unsigned char packet[300], destination[4] = {255,255,255,255};
    unsigned short offset = 240;
    unsigned long seconds = (net->now - dhcp->started) / 1000UL;
    memset(packet, 0, sizeof(packet));
    packet[0] = 1; packet[1] = 1; packet[2] = 6;
    dhcp_write32(packet + 4, dhcp->xid);
    if (type != 4) net_write16(packet + 8, seconds > 65535UL ? 65535U : seconds);
    if (dhcp->state == NET_DHCP_RENEWING || dhcp->state == NET_DHCP_REBINDING) {
        memcpy(packet + 12, net->config.address, 4);
        if (dhcp->state == NET_DHCP_RENEWING) memcpy(destination, dhcp->server, 4);
    } else if (type != 4) net_write16(packet + 10, 0x8000U);
    memcpy(packet + 28, net->mac, 6);
    packet[236] = 99; packet[237] = 130; packet[238] = 83; packet[239] = 99;
    packet[offset++] = 53; packet[offset++] = 1; packet[offset++] = type;
    packet[offset++] = 61; packet[offset++] = 7; packet[offset++] = 1;
    memcpy(packet + offset, net->mac, 6); offset += 6;
    if (type == 4 || (type == 3 && dhcp->state == NET_DHCP_REQUESTING)) {
        packet[offset++] = 50; packet[offset++] = 4;
        memcpy(packet + offset, dhcp->offered.address, 4); offset += 4;
        packet[offset++] = 54; packet[offset++] = 4;
        memcpy(packet + offset, dhcp->server, 4); offset += 4;
    }
    if (type != 4) {
        packet[offset++] = 55; packet[offset++] = 6;
        packet[offset++] = 1; packet[offset++] = 3; packet[offset++] = 6;
        packet[offset++] = 51; packet[offset++] = 58; packet[offset++] = 59;
        packet[offset++] = 57; packet[offset++] = 2; net_write16(packet + offset, 576); offset += 2;
    }
    packet[offset++] = 255;
    /* Pad to the BOOTP minimum without including it in the option parser. */
    return net_udp_send(net, destination, 68, 67, packet, sizeof(packet));
}
