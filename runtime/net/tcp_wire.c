#include "tcp_internal.h"

unsigned long tcp_read32(const unsigned char *p) {
    return (unsigned long)net_read16(p) * 65536UL + net_read16(p + 2);
}
void tcp_write32(unsigned char *p, unsigned long value) {
    net_write16(p, value / 65536UL); net_write16(p + 2, value % 65536UL);
}
int tcp_before(unsigned long a, unsigned long b) { return a != b && a - b >= 0x80000000UL; }
void net_tcp_pending_dropped(NetContext *net) {
    NetTcp *tcp = net->tcp_pending;
    if (!tcp) return;
    net->tcp_pending = 0; tcp->queued = 0;
    if (tcp->queued_flight) tcp->flight_sent = 0;
    else tcp->ack_pending = 1;
}
void net_tcp_transmitted(NetContext *net) {
    NetTcp *tcp = net->tcp_pending;
    unsigned long end;
    unsigned short length, flags, repeated;
    if (!tcp) return;
    net->tcp_pending = 0; tcp->queued = 0;
    if (tcp->queued_flight) {
        length = net_read16(net->tx + 16) - 20 - (net->tx[46] / 16) * 4;
        flags = net->tx[47];
        end = tcp_read32(net->tx + 38) + length + ((flags & (TCP_SYN | TCP_FIN)) ? 1 : 0);
        repeated = tcp_before(tcp_read32(net->tx + 38), tcp->sent_nxt);
        if (tcp_before(tcp->sent_nxt, end)) tcp->sent_nxt = end;
        if (!tcp->flight_sent) tcp->sent_at = net->now;
        tcp->flight_sent = 1;
        if (repeated) net_count(&tcp->retransmits);
        else if (!tcp->sample_valid) { tcp->sample_at = net->now; tcp->sample_end = end; tcp->sample_valid = 1; }
    }
}
int tcp_send(NetTcp *tcp, unsigned long sequence, unsigned short flags,
             const unsigned char *data, unsigned short length, unsigned short reliable) {
    NetContext *net = tcp->net;
    unsigned char mac[6], *p;
    unsigned short header = (flags & TCP_SYN) ? 24 : 20;
    int status;
    if (net->pending) return NET_AGAIN;
    status = net_route(net, tcp->peer, mac);
    if (status) return status;
    net_ethernet(net, mac, 0x0800); net_ipv4_header(net, tcp->peer, 6, header + length);
    p = net->tx + 34; memset(p, 0, header);
    net_write16(p, tcp->local_port); net_write16(p + 2, tcp->remote_port);
    tcp_write32(p + 4, sequence); if (flags & TCP_ACK) tcp_write32(p + 8, tcp->rcv_nxt);
    p[12] = header * 4; p[13] = flags; net_write16(p + 14, NET_TCP_RX_CAP(tcp) - tcp->rx_used);
    if (flags & TCP_SYN) { p[20] = 2; p[21] = 4; net_write16(p + 22, NET_TCP_MSS); }
    if (length) memcpy(p + header, data, length);
    net_write16(p + 16, net_l4_checksum(net->config.address, tcp->peer, 6, p, header + length));
    tcp->queued = 1; tcp->queued_flight = reliable; net->tcp_pending = tcp;
    status = net_transmit(net, 34 + header + length);
    if (status) { if (net->tcp_pending == tcp) net->tcp_pending = 0; tcp->queued = 0; }
    return status;
}
