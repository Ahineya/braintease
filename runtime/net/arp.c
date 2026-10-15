#include "internal.h"

int net_arp_send(NetContext *net, unsigned short operation, const unsigned char *mac, const unsigned char *address) {
    unsigned short i;
    net_ethernet(net, mac, 0x0806);
    net_write16(net->tx + 14, 1); net_write16(net->tx + 16, 0x0800);
    net->tx[18] = 6; net->tx[19] = 4; net_write16(net->tx + 20, operation);
    memcpy(net->tx + 22, net->mac, 6); memcpy(net->tx + 28, net->config.address, 4);
    for (i = 0; i < 6; ++i) net->tx[32 + i] = operation == 1 ? 0 : mac[i];
    memcpy(net->tx + 38, address, 4);
    return net_transmit(net, 42);
}
int net_arp_input(NetContext *net, unsigned short length) {
    unsigned char *p = net->rx;
    unsigned short operation, i;
    if (length < 42 || net_read16(p + 14) != 1 || net_read16(p + 16) != 0x0800 || p[18] != 6 || p[19] != 4) { ++net->rejected; return NET_REJECTED; }
    operation = net_read16(p + 20);
    if ((operation != 1 && operation != 2) || memcmp(p + 38, net->config.address, 4) || memcmp(p + 6, p + 22, 6)) return NET_OK;
    if (p[28] && p[28] < 224 && !(p[22] & 1)) {
        for (i = 0; i < NET_ARP_ENTRIES; ++i) {
            if (net->arp[i].used && !memcmp(net->arp[i].address, p + 28, 4)) {
                memcpy(net->arp[i].mac, p + 22, 6); net->arp[i].ready = 1;
            }
        }
    }
    if (operation == 1) return net_arp_send(net, 2, p + 22, p + 28);
    return NET_OK;
}
int net_route(NetContext *net, const unsigned char *destination, unsigned char *mac) {
    const unsigned char *hop = destination;
    NetArpEntry *entry;
    unsigned short i;
    if (!destination[0] || destination[0] >= 224 || destination[0] == 127) return NET_BAD_ARGUMENT;
    for (i = 0; i < 4; ++i) {
        if ((destination[i] & net->config.mask[i]) != (net->config.address[i] & net->config.mask[i])) { hop = net->config.gateway; break; }
    }
    if (!hop[0]) return NET_UNREACHABLE;
    for (i = 0; i < NET_ARP_ENTRIES; ++i) if (net->arp[i].used && !memcmp(net->arp[i].address, hop, 4)) break;
    if (i == NET_ARP_ENTRIES) {
        i = net->next_arp; net->next_arp = (i + 1) % NET_ARP_ENTRIES;
        entry = &net->arp[i]; memset(entry, 0, sizeof(*entry));
        entry->used = 1; memcpy(entry->address, hop, 4);
    }
    entry = &net->arp[i];
    if (entry->ready) { memcpy(mac, entry->mac, 6); return NET_OK; }
    if (!entry->attempts || net->now - entry->requested >= 1000UL) {
        if (entry->attempts == 3) return NET_UNREACHABLE;
        if (net->pending) return NET_AGAIN;
        if (net_arp_send(net, 1, 0, hop)) return NET_IO_ERROR;
        ++entry->attempts; entry->requested = net->now;
    }
    return NET_AGAIN;
}
