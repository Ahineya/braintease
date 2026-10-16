#include "dhcp_internal.h"

static int lease_config(NetDhcp *dhcp, const DhcpReply *reply, NetConfig *config) {
    unsigned short i, host_bits = 0, all_zero = 1, all_ones = 1, bits, value;
    *config = dhcp->offered;
    if (reply->config.address[0]) memcpy(config->address, reply->config.address, 4);
    if (reply->seen & DHCP_MASK) memcpy(config->mask, reply->config.mask, 4);
    if (reply->seen & DHCP_ROUTER) memcpy(config->gateway, reply->config.gateway, 4);
    if (reply->seen & DHCP_DNS) memcpy(config->dns, reply->config.dns, 4);
    if (!dhcp_ip(config->address) || !config->mask[0] || !net_config_valid(config)) return 0;
    for (i = 0; i < 4; ++i) {
        bits = 255U - config->mask[i]; value = config->address[i] & bits;
        if (bits) host_bits = 1;
        if (value) all_zero = 0;
        if (value != bits) all_ones = 0;
    }
    return !host_bits || (!all_zero && !all_ones);
}
void dhcp_receive(NetContext *net, const NetDatagram *packet, void *user) {
    NetDhcp *dhcp = user;
    DhcpReply reply;
    NetConfig config;
    unsigned long t1, t2, elapsed;
    unsigned short same, configured;
    int status;
    if (packet->source_port != 67 || !dhcp_ip(packet->source) || packet->length < 240 ||
        dhcp_read32(packet->data + 4) != dhcp->xid || memcmp(packet->data + 28, net->mac, 6)) return;
    if (!dhcp_decode(packet->data, packet->length, &reply)) { ++dhcp->malformed; return; }
    if (dhcp->state == NET_DHCP_SELECTING) {
        if (reply.type != 2) return;
        if (!(reply.seen & DHCP_LEASE) || !reply.lease || !lease_config(dhcp, &reply, &config)) { ++dhcp->malformed; return; }
        dhcp->offered = config; memcpy(dhcp->server, reply.server, 4);
        dhcp->state = NET_DHCP_REQUESTING; dhcp->attempts = 0; dhcp->delay = 0; dhcp->action_at = net->now;
        dhcp->request_sent = 0;
        return;
    }
    if (dhcp->state != NET_DHCP_REQUESTING && dhcp->state != NET_DHCP_RENEWING && dhcp->state != NET_DHCP_REBINDING) return;
    if (dhcp->state != NET_DHCP_REBINDING && memcmp(reply.server, dhcp->server, 4)) return;
    if (!dhcp->request_sent) return;
    if (reply.type == 6) {
        configured = net->config.address[0] != 0;
        net_unconfigure(net); dhcp->generation = net->generation;
        dhcp->state = NET_DHCP_BACKOFF; dhcp->delay = 1000; dhcp->action_at = net->now;
        if (configured) dhcp->started = net->now;
        dhcp->lease = 0; dhcp->error = NET_UNREACHABLE;
        return;
    }
    if (reply.type != 5) return;
    if (!(reply.seen & DHCP_LEASE) || !reply.lease || !lease_config(dhcp, &reply, &config) ||
        (dhcp->state == NET_DHCP_REQUESTING && (!dhcp_ip(reply.config.address) || memcmp(config.address, dhcp->offered.address, 4)))) { ++dhcp->malformed; return; }
    t1 = (reply.seen & DHCP_T1) ? reply.t1 : reply.lease / 2;
    t2 = (reply.seen & DHCP_T2) ? reply.t2 : reply.lease - reply.lease / 8;
    if (reply.lease == 0xffffffffUL) { t1 = reply.lease; t2 = reply.lease; }
    else if (!t1 || t1 >= t2 || t2 >= reply.lease) { ++dhcp->malformed; return; }
    elapsed = net->now - dhcp->request_at;
    if (reply.lease != 0xffffffffUL && elapsed / 1000UL >= reply.lease) { ++dhcp->malformed; return; }
    same = net->config.address[0] && !memcmp(net->config.address, config.address, 4);
    dhcp->offered = config; memcpy(dhcp->server, reply.server, 4);
    dhcp->lease = reply.lease; dhcp->t1 = t1; dhcp->t2 = t2;
    dhcp->age = elapsed / 1000UL; dhcp->fraction = elapsed % 1000UL; dhcp->last_ms = net->now;
    dhcp->error = NET_OK;
    if (same) {
        status = net_configure(net, &config);
        if (status) {
            if (status == NET_LINK_DOWN) dhcp_begin(dhcp, net->now);
            else dhcp_fail(dhcp, status);
            return;
        }
        dhcp->generation = net->generation; memcpy(net->watch, config.address, 4);
        dhcp->state = NET_DHCP_BOUND; ++dhcp->renewals;
    } else {
        net_unconfigure(net); dhcp->generation = net->generation;
        memcpy(net->watch, config.address, 4);
        dhcp->state = NET_DHCP_PROBING; dhcp->probes = 0;
        dhcp->action_at = net->now; dhcp->delay = net_random(net) % 1001U;
    }
}
