#include "internal.h"

int net_config_valid(const NetConfig *config) {
    unsigned short i, tail = 0, octet;
    if (!config || !config->address[0] || config->address[0] >= 224 || config->address[0] == 127) return 0;
    for (i = 0; i < 4; ++i) {
        octet = config->mask[i];
        if (tail && octet) return 0;
        if (octet != 255) {
            if (octet && octet != 254 && octet != 252 && octet != 248 && octet != 240 && octet != 224 && octet != 192 && octet != 128) return 0;
            tail = 1;
        }
    }
    return 1;
}
void net_unconfigure(NetContext *net) {
    memset(net->config.address, 0, 16);
    net_reset_paths(net);
    memset(net->watch, 0, 4);
    net->conflict = 0;
    net->event = NET_EVENT_NONE; ++net->generation;
}
int net_configure(NetContext *net, const NetConfig *config) {
    NetConfig saved;
    if (!net || !net_config_valid(config) || config->interface != net->config.interface) return NET_BAD_ARGUMENT;
    if (!memcmp(net->config.address, config->address, 16)) return NET_OK;
    saved = *config;
    net_unconfigure(net);
    net->config = saved;
    return net->link ? net_arp_send(net, 1, 0, net->config.address) : NET_OK;
}
int net_open(NetContext *net, unsigned short interface, unsigned short seed) {
    VmNetInfo info;
    if (!net) return NET_BAD_ARGUMENT;
    if (vm_net_info(interface, &info) || info.mtu != 1500 || info.max_frame != NET_FRAME_SIZE) return NET_IO_ERROR;
    memset(net, 0, sizeof(*net));
    net->config.interface = interface; net->config.seed = seed;
    memcpy(net->mac, info.mac, 6); net->random = seed; net->generation = 1;
    net->link = info.link != 0; net->link_epoch = 1;
    return NET_OK;
}
