#include "internal.h"

void net_arp_invalidate(NetContext *net, const unsigned char *address) {
    unsigned short i;
    for (i = 0; i < NET_ARP_ENTRIES; ++i) {
        if (!address || !memcmp(net->arp[i].address, address, 4)) {
            if (net->pending && (!address || (net->arp[i].used && net->arp[i].ready && !memcmp(net->tx, net->arp[i].mac, 6)) ||
                (net_read16(net->tx + 12) == 0x0806 && !memcmp(net->tx + 38, address, 4)))) {
                net->pending = 0; net_count(&net->counters.tx_discarded);
                if (net->tcp_driver && net->tcp_pending) net->tcp_driver->dropped(net);
            }
            memset(&net->arp[i], 0, sizeof(net->arp[i]));
        }
    }
}
void net_arp_sent(NetContext *net) {
    unsigned short i;
    if (net_read16(net->tx + 12) != 0x0806 || net_read16(net->tx + 20) != 1) return;
    for (i = 0; i < NET_ARP_ENTRIES; ++i) {
        if (net->arp[i].used && !net->arp[i].ready && !memcmp(net->arp[i].address, net->tx + 38, 4)) {
            net->arp[i].requested = net->now; net_count(&net->counters.arp_queries); return;
        }
    }
}
void net_arp_tick(NetContext *net) {
    unsigned short i;
    NetArpEntry *entry;
    for (i = 0; i < NET_ARP_ENTRIES; ++i) {
        entry = &net->arp[i];
        if (!entry->used) continue;
        /* Local backpressure has not put this probe on the wire yet. */
        if (net->pending && net_read16(net->tx + 12) == 0x0806 && !memcmp(net->tx + 38, entry->address, 4)) continue;
        if (entry->ready && net->now - entry->resolved >= NET_ARP_TTL_MS) {
            entry->ready = 0; entry->attempts = 0;
            net_count(&net->counters.arp_expired);
        }
        if (!entry->ready && !entry->failed && entry->attempts == 3 && net->now - entry->requested >= 4000UL) {
            entry->failed = 1; entry->requested = net->now;
            net_count(&net->counters.arp_failed);
        }
        if (entry->failed && net->now - entry->requested >= NET_ARP_COOLDOWN_MS) {
            entry->failed = 0; entry->attempts = 0;
        }
    }
}
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
    if ((operation == 1 || operation == 2) && !(p[22] & 1) && memcmp(p + 22, net->mac, 6) &&
        !memcmp(p + 6, p + 22, 6) && net->watch[0] &&
        (!memcmp(p + 28, net->watch, 4) ||
         (!net->config.address[0] && !p[28] && !p[29] && !p[30] && !p[31] && !memcmp(p + 38, net->watch, 4)))) net->conflict = 1;
    /* A watched address is relinquished by DHCP after this frame. Do not
     * answer a conflicting probe by claiming that address in the meantime. */
    if (net->conflict || !net->config.address[0]) return NET_OK;
    if ((operation != 1 && operation != 2) || memcmp(p + 38, net->config.address, 4) || memcmp(p + 6, p + 22, 6)) return NET_OK;
    if (p[28] && p[28] < 224 && !(p[22] & 1)) {
        for (i = 0; i < NET_ARP_ENTRIES; ++i) {
            if (net->arp[i].used && !memcmp(net->arp[i].address, p + 28, 4)) {
                memcpy(net->arp[i].mac, p + 22, 6); net->arp[i].ready = 1;
                net->arp[i].resolved = net->now; net->arp[i].attempts = 0; net->arp[i].failed = 0;
                net_count(&net->counters.arp_updates);
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
    if (!net->link) return NET_LINK_DOWN;
    if (!net->config.address[0]) return NET_UNREACHABLE;
    net_arp_tick(net);
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
    if (entry->failed) return NET_UNREACHABLE;
    if (!entry->attempts || net->now - entry->requested >= (1000UL << (entry->attempts - 1))) {
        if (net->pending) return NET_AGAIN;
        i = net_arp_send(net, 1, 0, hop);
        if (i) return i;
        ++entry->attempts; entry->requested = net->now;
    }
    return NET_AGAIN;
}
