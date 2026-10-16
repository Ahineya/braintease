#include "dhcp_internal.h"

void dhcp_begin(NetDhcp *dhcp, unsigned long now_ms) {
    NetContext *net = dhcp->net;
    net_unconfigure(net); dhcp->generation = net->generation;
    dhcp->offered = net->config;
    dhcp->xid = (unsigned long)net_random(net) * 65536UL + net_random(net);
    dhcp->state = NET_DHCP_SELECTING; dhcp->started = now_ms;
    dhcp->attempts = 0; dhcp->delay = 1000UL + net_random(net) % 9001U;
    dhcp->action_at = now_ms; dhcp->lease = 0; dhcp->request_sent = 0;
    dhcp->link_epoch = net->link_epoch;
    dhcp->error = net->link ? NET_OK : NET_LINK_DOWN;
    if (!net->link) dhcp->state = NET_DHCP_WAIT_LINK;
}
int dhcp_fail(NetDhcp *dhcp, int error) {
    net_unconfigure(dhcp->net); dhcp->generation = dhcp->net->generation;
    net_udp_unbind(dhcp->net, 68); dhcp->net->dhcp = 0;
    dhcp->state = NET_DHCP_FAILED; dhcp->active = 0; dhcp->error = error;
    return error;
}
int net_dhcp_start(NetContext *net, NetDhcp *dhcp, unsigned long now_ms, unsigned long timeout_ms) {
    int status;
    if (!net || !dhcp || dhcp->active || net->dhcp || !timeout_ms || timeout_ms > 600000UL) return NET_BAD_ARGUMENT;
    status = net_udp_bind(net, 68, dhcp_receive, dhcp);
    if (status) return status;
    memset(dhcp, 0, sizeof(*dhcp));
    dhcp->net = net; dhcp->active = 1; dhcp->timeout = timeout_ms;
    net->dhcp = 1; dhcp_begin(dhcp, now_ms);
    return NET_OK;
}
void net_dhcp_stop(NetDhcp *dhcp) {
    if (!dhcp || !dhcp->active) return;
    net_unconfigure(dhcp->net); net_udp_unbind(dhcp->net, 68);
    dhcp->net->dhcp = 0; dhcp->active = 0; dhcp->state = NET_DHCP_STOPPED;
}
static void age_lease(NetDhcp *dhcp, unsigned long now_ms) {
    unsigned long elapsed = now_ms - dhcp->last_ms, seconds;
    dhcp->last_ms = now_ms;
    dhcp->fraction += elapsed % 1000UL;
    seconds = elapsed / 1000UL + dhcp->fraction / 1000UL;
    dhcp->fraction %= 1000UL;
    if (0xffffffffUL - dhcp->age < seconds) dhcp->age = 0xffffffffUL;
    else dhcp->age += seconds;
}
static int progress(NetDhcp *dhcp, unsigned long now_ms) {
    NetContext *net = dhcp->net;
    unsigned long remaining, began;
    int status;
    if (dhcp->state == NET_DHCP_BACKOFF) {
        if (dhcp->conflicts >= 3 && !net->pending) return NET_REJECTED;
        if (now_ms - dhcp->action_at >= dhcp->delay) {
            began = dhcp->started; dhcp_begin(dhcp, now_ms); dhcp->started = began;
        }
        return NET_OK;
    }
    if (dhcp->state == NET_DHCP_PROBING) return dhcp_probe(dhcp, now_ms);
    if (dhcp->state == NET_DHCP_BOUND) {
        if (dhcp->announced && !net->pending && now_ms - dhcp->action_at >= 2000UL) {
            status = net_arp_send(net, 1, 0, net->config.address);
            if (status) return status;
            dhcp->announced = 0;
        }
        return NET_OK;
    }
    if (net->pending || now_ms - dhcp->action_at < dhcp->delay) return NET_OK;
    if ((dhcp->state == NET_DHCP_RENEWING || dhcp->state == NET_DHCP_REBINDING) && dhcp->age < dhcp->retry_age) return NET_OK;
    status = dhcp_send(dhcp, dhcp->state == NET_DHCP_SELECTING ? 1 : 3);
    if (status == NET_AGAIN) return NET_OK;
    if (status == NET_UNREACHABLE && dhcp->state == NET_DHCP_RENEWING) {
        dhcp->action_at = now_ms; dhcp->delay = 1000; return NET_OK;
    }
    if (status) return status;
    if (dhcp->state != NET_DHCP_SELECTING && !dhcp->request_sent) {
        dhcp->request_at = now_ms; dhcp->request_sent = 1;
    }
    dhcp->action_at = now_ms;
    if (dhcp->state == NET_DHCP_RENEWING || dhcp->state == NET_DHCP_REBINDING) {
        remaining = (dhcp->state == NET_DHCP_RENEWING ? dhcp->t2 : dhcp->lease) - dhcp->age;
        remaining /= 2;
        if (remaining < 60) remaining = 60;
        dhcp->retry_age = 0xffffffffUL - dhcp->age < remaining ? 0xffffffffUL : dhcp->age + remaining;
        dhcp->delay = 0;
    } else {
        remaining = 4000UL;
        if (dhcp->attempts < 4) remaining <<= dhcp->attempts;
        else remaining = 64000UL;
        dhcp->delay = remaining + net_random(net) % 1001U;
        if (dhcp->attempts < 5) ++dhcp->attempts;
    }
    return NET_OK;
}
int net_dhcp_poll(NetDhcp *dhcp, unsigned long now_ms) {
    NetContext *net;
    int status, result;
    if (!dhcp || !dhcp->active) return NET_BAD_ARGUMENT;
    net = dhcp->net; net->now = now_ms;
    if (net->generation != dhcp->generation) {
        /* A caller explicitly installed another configuration. Stop owning DHCP
         * without deleting that replacement address. */
        net_udp_unbind(net, 68); net->dhcp = 0; memset(net->watch, 0, 4);
        dhcp->state = NET_DHCP_FAILED; dhcp->active = 0; dhcp->error = NET_CONFIG_CHANGED;
        return NET_CONFIG_CHANGED;
    }
    status = net_link_poll(net, now_ms);
    if (status == NET_IO_ERROR) return dhcp_fail(dhcp, status);
    if (net->link_epoch != dhcp->link_epoch) dhcp_begin(dhcp, now_ms);
    if (!net->link) return NET_LINK_DOWN;
    if (dhcp->lease) {
        age_lease(dhcp, now_ms);
        if (dhcp->lease != 0xffffffffUL && dhcp->age >= dhcp->lease) {
            ++dhcp->expirations; dhcp_begin(dhcp, now_ms); dhcp->error = NET_TIMEOUT;
        } else if (dhcp->lease != 0xffffffffUL) {
            if (dhcp->age >= dhcp->t1 && dhcp->state == NET_DHCP_BOUND) {
                dhcp->state = NET_DHCP_RENEWING; dhcp->action_at = now_ms; dhcp->delay = 0;
                dhcp->started = now_ms; dhcp->request_sent = 0; dhcp->retry_age = 0;
                dhcp->xid = (unsigned long)net_random(net) * 65536UL + net_random(net);
            }
            if (dhcp->age >= dhcp->t2 && dhcp->state == NET_DHCP_RENEWING) {
                dhcp->state = NET_DHCP_REBINDING; dhcp->action_at = now_ms; dhcp->delay = 0; dhcp->retry_age = 0;
            }
        }
    }
    if ((dhcp->state < NET_DHCP_BOUND || dhcp->state == NET_DHCP_BACKOFF) && now_ms - dhcp->started >= dhcp->timeout) return dhcp_fail(dhcp, NET_TIMEOUT);
    result = net_poll(net, now_ms);
    if (result == NET_IO_ERROR) return dhcp_fail(dhcp, result);
    if (net->link_epoch != dhcp->link_epoch) { dhcp_begin(dhcp, now_ms); return result; }
    if (!net->link) return NET_LINK_DOWN;
    if (!dhcp->active) return dhcp->error;
    status = dhcp_conflict(dhcp, now_ms);
    if (!status) status = progress(dhcp, now_ms);
    if (status == NET_LINK_DOWN) { dhcp_begin(dhcp, now_ms); return status; }
    if (status) return dhcp_fail(dhcp, status);
    return result;
}
