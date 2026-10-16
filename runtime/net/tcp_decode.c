#include "tcp_internal.h"

static int options(const unsigned char *p, unsigned short length, unsigned short *mss) {
    unsigned short offset = 20, code, size, seen = 0;
    *mss = 536;
    while (offset < length) {
        code = p[offset++];
        if (!code) return 1;
        if (code == 1) continue;
        if (offset == length) return 0;
        size = p[offset++];
        if (size < 2 || size - 2 > length - offset) return 0;
        if (code == 2) {
            if (size != 4 || seen || !net_read16(p + offset)) return 0;
            *mss = net_read16(p + offset); seen = 1;
        }
        offset += size - 2;
    }
    return 1;
}
int tcp_decode(NetContext *net, unsigned short header, unsigned short length, TcpSegment *segment) {
    const unsigned char *p = net->rx + 14 + header;
    unsigned short tcp_header;
    if (length < 20 || memcmp(net->rx, net->mac, 6) || !net->rx[26] || net->rx[26] >= 224 || net->rx[26] == 127) return 0;
    tcp_header = (p[12] / 16) * 4;
    if (tcp_header < 20 || tcp_header > 60 || tcp_header > length || !net_read16(p) || !net_read16(p + 2) ||
        net_l4_checksum(net->rx + 26, net->rx + 30, 6, p, length) ||
        ((p[13] & (TCP_SYN | TCP_FIN)) == (TCP_SYN | TCP_FIN)) || !options(p, tcp_header, &segment->mss)) return 0;
    segment->source = net->rx + 26;
    segment->source_port = net_read16(p); segment->destination_port = net_read16(p + 2);
    segment->sequence = tcp_read32(p + 4); segment->acknowledgement = tcp_read32(p + 8);
    segment->window = net_read16(p + 14); segment->flags = p[13] & 63;
    segment->data = p + tcp_header; segment->length = length - tcp_header;
    return 1;
}
int tcp_reset_segment(NetContext *net, const TcpSegment *segment) {
    unsigned char *p;
    unsigned short flags = TCP_RST;
    unsigned long sequence = 0, acknowledgement = 0;
    if (segment->flags & TCP_RST) return NET_OK;
    if (net->tcp_reset_sent && net->now - net->tcp_reset_at < 100UL) return NET_OK;
    if (segment->flags & TCP_ACK) sequence = segment->acknowledgement;
    else {
        flags |= TCP_ACK;
        acknowledgement = segment->sequence + segment->length + ((segment->flags & (TCP_SYN | TCP_FIN)) ? 1 : 0);
    }
    net_ethernet(net, net->rx + 6, 0x0800); net_ipv4_header(net, segment->source, 6, 20);
    p = net->tx + 34; memset(p, 0, 20);
    net_write16(p, segment->destination_port); net_write16(p + 2, segment->source_port);
    tcp_write32(p + 4, sequence); tcp_write32(p + 8, acknowledgement); p[12] = 80; p[13] = flags;
    net_write16(p + 16, net_l4_checksum(net->config.address, segment->source, 6, p, 20));
    net->tcp_reset_sent = 1; net->tcp_reset_at = net->now;
    return net_transmit(net, 54);
}
int net_tcp_input(NetContext *net, unsigned short header, unsigned short length) {
    TcpSegment segment;
    NetTcp *tcp;
    unsigned short i;
    if (!tcp_decode(net, header, length, &segment)) { ++net->rejected; return NET_REJECTED; }
    for (i = 0; i < NET_TCP_CONNECTIONS; ++i) {
        tcp = net->tcp[i];
        if (tcp && tcp->local_port == segment.destination_port && tcp->remote_port == segment.source_port && !memcmp(tcp->peer, segment.source, 4)) {
            tcp->now = net->now;
            if (!tcp_lifecycle(tcp)) tcp_receive(tcp, &segment);
            return NET_OK;
        }
    }
    if (tcp_wait_input(net, &segment)) return NET_OK;
    if (net->tcp_listen_driver && net->tcp_listen_driver->input(net, &segment)) return NET_OK;
    return tcp_reset_segment(net, &segment);
}
