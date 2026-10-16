#include "dhcp_internal.h"

int dhcp_conflict(NetDhcp *dhcp, unsigned long now_ms) {
    NetContext *net = dhcp->net;
    unsigned short configured = net->config.address[0] != 0;
    if (!net->conflict) return NET_OK;
    net->conflict = 0;
    ++dhcp->conflicts;
    /* Give up this address immediately. DECLINE uses the unconfigured source;
     * its queued frame must survive entry into the bounded backoff state. */
    net_unconfigure(net); dhcp->generation = net->generation;
    dhcp->state = NET_DHCP_BACKOFF; dhcp->lease = 0;
    dhcp->action_at = now_ms; dhcp->delay = 10000;
    if (configured) dhcp->started = now_ms;
    dhcp->error = NET_REJECTED;
    return dhcp_send(dhcp, 4);
}
int dhcp_probe(NetDhcp *dhcp, unsigned long now_ms) {
    NetContext *net = dhcp->net;
    int status;
    if (now_ms - dhcp->action_at < dhcp->delay || net->pending) return NET_OK;
    if (dhcp->probes < 3) {
        /* net remains unconfigured: SPA 0, THA 0, candidate TPA. */
        status = net_arp_send(net, 1, 0, dhcp->offered.address);
        if (status) return status;
        ++dhcp->probes; dhcp->action_at = now_ms;
        dhcp->delay = dhcp->probes == 3 ? 2000UL : 1000UL + net_random(net) % 1001U;
        return NET_OK;
    }
    status = net_configure(net, &dhcp->offered);
    if (status) return status;
    memcpy(net->watch, dhcp->offered.address, 4);
    dhcp->generation = net->generation; dhcp->state = NET_DHCP_BOUND;
    dhcp->announced = 1; dhcp->action_at = now_ms; ++dhcp->acquired;
    return NET_OK;
}
