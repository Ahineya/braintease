#include "internal.h"

int net_init(NetContext *net, const NetConfig *config) {
    NetConfig saved;
    int status;
    if (!net || !net_config_valid(config)) return NET_BAD_ARGUMENT;
    saved = *config;
    status = net_open(net, saved.interface, saved.seed);
    if (status) return status;
    return net_configure(net, &saved);
}
int net_poll(NetContext *net, unsigned long now_ms) {
    VmNetReceive result;
    unsigned short type;
    int status;
    net->now = now_ms; net->event = NET_EVENT_NONE;
    status = net_link_poll(net, now_ms);
    if (status) return status;
    net_arp_tick(net);
    net_history_tick(net);
    if (net->pending) {
        status = net_transmit(net, net->pending);
        if (status) return status;
        if (net->pending) return NET_AGAIN;
    }
    if (vm_net_rx(net->config.interface, net->rx, NET_FRAME_SIZE, &result)) return NET_IO_ERROR;
    if (result.outcome == VM_NET_EMPTY) return NET_AGAIN;
    if (result.outcome == VM_NET_LINK_DOWN) { net_link_lost(net); return NET_LINK_DOWN; }
    if (result.outcome != VM_NET_OK) return NET_IO_ERROR;
    if (result.length < 14) { ++net->rejected; return NET_REJECTED; }
    if (memcmp(net->rx, net->mac, 6)) {
        unsigned short i;
        for (i = 0; i < 6; ++i) if (net->rx[i] != 255) return NET_OK;
    }
    type = net_read16(net->rx + 12);
    if (type == 0x0806) return net_arp_input(net, result.length);
    if (type == 0x0800) return net_ipv4_input(net, result.length);
    return NET_OK;
}
