#include "internal.h"

int net_udp_bind(NetContext *net, unsigned short port, NetUdpHandler handler, void *user) {
    unsigned short i, free_slot = NET_UDP_PORTS;
    if (!port || !handler) return NET_BAD_ARGUMENT;
    for (i = 0; i < NET_UDP_PORTS; ++i) {
        if (net->ports[i].port == port) return NET_NO_PORT;
        if (!net->ports[i].port) free_slot = i;
    }
    if (free_slot == NET_UDP_PORTS) return NET_NO_PORT;
    net->ports[free_slot].port = port; net->ports[free_slot].handler = handler; net->ports[free_slot].user = user;
    return NET_OK;
}
int net_udp_unbind(NetContext *net, unsigned short port) {
    unsigned short i;
    for (i = 0; i < NET_UDP_PORTS; ++i) if (net->ports[i].port == port && port) { net->ports[i].port = 0; return NET_OK; }
    return NET_NO_PORT;
}
int net_udp_send(NetContext *net, const unsigned char *destination, unsigned short source_port,
                 unsigned short destination_port, const unsigned char *data, unsigned short length) {
    unsigned char mac[6];
    unsigned short checksum;
    int status;
    if (!destination || !destination_port || length > 1472 || (length && !data)) return NET_BAD_ARGUMENT;
    if (net->pending) return NET_AGAIN;
    status = net_route(net, destination, mac); if (status) return status;
    net_ethernet(net, mac, 0x0800); net_ipv4_header(net, destination, 17, length + 8);
    net_write16(net->tx + 34, source_port); net_write16(net->tx + 36, destination_port);
    net_write16(net->tx + 38, length + 8); net_write16(net->tx + 40, 0);
    if (length) memcpy(net->tx + 42, data, length);
    checksum = net_udp_checksum(net->config.address, destination, net->tx + 34, length + 8);
    net_write16(net->tx + 40, checksum ? checksum : 65535U);
    return net_transmit(net, length + 42);
}
int net_udp_input(NetContext *net, unsigned short header, unsigned short length) {
    unsigned char *udp = net->rx + 14 + header;
    unsigned short size, destination, i;
    NetDatagram packet;
    if (length < 8) { ++net->rejected; return NET_REJECTED; }
    size = net_read16(udp + 4); destination = net_read16(udp + 2);
    if (size < 8 || size > length || !destination || (net_read16(udp + 6) && net_udp_checksum(net->rx + 26, net->config.address, udp, size))) {
        ++net->rejected; return NET_REJECTED;
    }
    for (i = 0; i < NET_UDP_PORTS; ++i) if (net->ports[i].port == destination) {
        memcpy(packet.source, net->rx + 26, 4); packet.source_port = net_read16(udp);
        packet.destination_port = destination; packet.data = udp + 8; packet.length = size - 8;
        net->ports[i].handler(net, &packet, net->ports[i].user);
        return NET_OK;
    }
    return NET_OK;
}
