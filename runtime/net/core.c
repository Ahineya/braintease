#include "internal.h"

int net_init(NetContext *net, const NetConfig *config) {
    VmNetInfo info;
    NetConfig saved;
    unsigned short i, tail = 0, octet;
    if (!net || !config || !config->address[0] || config->address[0] >= 224 || config->address[0] == 127) return NET_BAD_ARGUMENT;
    for (i = 0; i < 4; ++i) {
        octet = config->mask[i];
        if (tail && octet) return NET_BAD_ARGUMENT;
        if (octet != 255) {
            if (octet && octet != 254 && octet != 252 && octet != 248 && octet != 240 && octet != 224 && octet != 192 && octet != 128) return NET_BAD_ARGUMENT;
            tail = 1;
        }
    }
    if (vm_net_info(config->interface, &info) || !info.link || info.mtu != 1500 || info.max_frame != NET_FRAME_SIZE) return NET_IO_ERROR;
    saved = *config;
    memset(net, 0, sizeof(*net));
    net->config = saved; memcpy(net->mac, info.mac, 6); net->random = saved.seed;
    return net_arp_send(net, 1, 0, net->config.address);
}
int net_poll(NetContext *net, unsigned long now_ms) {
    VmNetReceive result;
    unsigned short type;
    net->now = now_ms; net->event = NET_EVENT_NONE;
    if (net->pending) {
        if (net_transmit(net, net->pending)) return NET_IO_ERROR;
        if (net->pending) return NET_AGAIN;
    }
    if (vm_net_rx(net->config.interface, net->rx, NET_FRAME_SIZE, &result)) return NET_IO_ERROR;
    if (result.outcome == VM_NET_EMPTY) return NET_AGAIN;
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
