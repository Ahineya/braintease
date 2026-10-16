#include "internal.h"

void net_history_tick(NetContext *net) {
    unsigned short i;
    for (i = 0; i < NET_TX_HISTORY; ++i) {
        if (net->sent[i].used && net->now - net->sent[i].sent >= NET_ERROR_TTL_MS) net->sent[i].used = 0;
    }
}
/* ICMP quotes must identify a packet actually accepted by the host, not just
 * a matching local port. This bounded window is correlation, not authentication. */
void net_remember_packet(NetContext *net, unsigned short length) {
    const unsigned char *p = net->tx;
    NetSentPacket *sent;
    unsigned short protocol;
    if (length < 42 || net_read16(p + 12) != 0x0800 || p[14] != 0x45 || !p[26] || p[30] >= 224 || !p[30]) return;
    protocol = p[23];
    if (protocol != 17 && protocol != 6 && !(protocol == 1 && p[34] == 8)) return;
    sent = &net->sent[net->next_sent]; net->next_sent = (net->next_sent + 1) % NET_TX_HISTORY;
    sent->used = 1; sent->protocol = protocol; sent->packet_id = net_read16(p + 18);
    sent->length = net_read16(p + 16); sent->sent = net->now;
    memcpy(sent->destination, p + 30, 4); memcpy(sent->prefix, p + 34, 8);
}
void net_forget_udp(NetContext *net, unsigned short port) {
    unsigned short i;
    for (i = 0; i < NET_TX_HISTORY; ++i) {
        if (net->sent[i].used && net->sent[i].protocol == 17 && net_read16(net->sent[i].prefix) == port) net->sent[i].used = 0;
    }
    if (net->pending >= 42 && net_read16(net->tx + 12) == 0x0800 && net->tx[23] == 17 && net_read16(net->tx + 34) == port) {
        net->pending = 0; net_count(&net->counters.tx_discarded);
    }
}
