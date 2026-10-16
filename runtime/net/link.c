#include "internal.h"

void net_count(unsigned long *counter) {
    if (*counter != 0xffffffffUL) ++*counter;
}
void net_reset_paths(NetContext *net) {
    if (net->pending) net_count(&net->counters.tx_discarded);
    net->pending = 0; net->next_arp = 0; net->next_sent = 0;
    memset(net->arp, 0, sizeof(net->arp));
    memset(net->sent, 0, sizeof(net->sent));
    memset(&net->error, 0, sizeof(net->error));
    net->event = NET_EVENT_NONE;
}
void net_link_lost(NetContext *net) {
    if (net->link) {
        net_reset_paths(net); net->link = 0; ++net->link_epoch;
        net_count(&net->counters.link_down); net->event = NET_EVENT_LINK_DOWN;
    }
    net->link_checked = net->now;
}
int net_link_poll(NetContext *net, unsigned long now_ms) {
    VmNetInfo info;
    unsigned short changed;
    net->now = now_ms;
    if (now_ms - net->link_checked < (net->link ? 1000UL : 250UL)) return net->link ? NET_OK : NET_LINK_DOWN;
    net->link_checked = now_ms;
    if (vm_net_info(net->config.interface, &info) || info.mtu != 1500 || info.max_frame != NET_FRAME_SIZE) return NET_IO_ERROR;
    if (!info.link) { net_link_lost(net); return NET_LINK_DOWN; }
    changed = memcmp(net->mac, info.mac, 6) != 0;
    if (!net->link || changed) {
        net_reset_paths(net); memcpy(net->mac, info.mac, 6);
        net->link = 1; ++net->link_epoch;
        net_count(&net->counters.link_up); net->event = NET_EVENT_LINK_UP;
        /* DHCP must discard its old lease before sending on a restored link. */
        if (!net->dhcp && net->config.address[0]) return net_arp_send(net, 1, 0, net->config.address);
    }
    return NET_OK;
}
