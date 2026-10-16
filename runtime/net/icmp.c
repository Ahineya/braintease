#include "internal.h"

int net_icmp_echo(NetContext *net, const unsigned char *destination, unsigned short identifier,
                  unsigned short sequence, const unsigned char *data, unsigned short length) {
    unsigned char mac[6];
    int status;
    if (!destination || length > 1472 || (length && !data)) return NET_BAD_ARGUMENT;
    if (net->pending) return NET_AGAIN;
    status = net_route(net, destination, mac); if (status) return status;
    net_ethernet(net, mac, 0x0800); net_ipv4_header(net, destination, 1, length + 8);
    net->tx[34] = 8; net->tx[35] = 0; net_write16(net->tx + 36, 0);
    net_write16(net->tx + 38, identifier); net_write16(net->tx + 40, sequence);
    if (length) memcpy(net->tx + 42, data, length);
    net_write16(net->tx + 36, net_checksum(net->tx + 34, length + 8));
    return net_transmit(net, length + 42);
}
int net_icmp_input(NetContext *net, unsigned short header, unsigned short length) {
    unsigned char *p = net->rx + 14 + header;
    if (length < 8 || net_checksum(p, length)) { ++net->rejected; return NET_REJECTED; }
    if (p[0] == 3 || p[0] == 11 || p[0] == 12) {
        if ((p[0] == 3 && p[1] > 15) || (p[0] == 11 && p[1] > 1) || (p[0] == 12 && p[1] > 2)) { ++net->rejected; return NET_REJECTED; }
        return net_icmp_error(net, p, length);
    }
    if (p[0] != 8 && p[0] != 0) return NET_OK;
    if (p[1]) { ++net->rejected; return NET_REJECTED; }
    net->event = p[0] == 8 ? NET_EVENT_ECHO_REQUEST : NET_EVENT_ECHO_REPLY;
    memcpy(net->peer, net->rx + 26, 4); net->identifier = net_read16(p + 4);
    net->sequence = net_read16(p + 6); net->payload = p + 8; net->payload_length = length - 8;
    if (p[0] == 8) {
        net_ethernet(net, net->rx + 6, 0x0800); net_ipv4_header(net, net->peer, 1, length);
        memcpy(net->tx + 34, p, length); net->tx[34] = 0; net_write16(net->tx + 36, 0);
        net_write16(net->tx + 36, net_checksum(net->tx + 34, length));
        ++net->icmp_requests;
        return net_transmit(net, length + 34);
    }
    return NET_OK;
}
