#include "tcp_internal.h"

static const NetTcpDriver driver = {net_tcp_input, net_tcp_transmitted, net_tcp_pending_dropped, net_tcp_error};
void net_tcp_enable(NetContext *net) { if (net) net->tcp_driver = &driver; }
void tcp_forget(NetTcp *tcp) {
    NetContext *net = tcp->net;
    unsigned short i;
    if (net->tcp_pending == tcp) {
        if (net->pending) net_count(&net->counters.tx_discarded);
        net->pending = 0; net->tcp_pending = 0;
    }
    tcp->queued = 0;
    for (i = 0; i < NET_TX_HISTORY; ++i) {
        if (net->sent[i].used && net->sent[i].protocol == 6 && net_read16(net->sent[i].prefix) == tcp->local_port &&
            net_read16(net->sent[i].prefix + 2) == tcp->remote_port && !memcmp(net->sent[i].destination, tcp->peer, 4)) net->sent[i].used = 0;
    }
}
void tcp_detach(NetTcp *tcp) {
    unsigned short i;
    tcp_forget(tcp);
    for (i = 0; i < NET_TCP_CONNECTIONS; ++i) if (tcp->net->tcp[i] == tcp) tcp->net->tcp[i] = 0;
    tcp->active = 0;
}
int tcp_fail(NetTcp *tcp, int error) {
    tcp_detach(tcp); tcp->state = NET_TCP_FAILED; tcp->error = error;
    tcp->tx_used = 0; tcp->rx_used = 0; tcp->inflight = 0;
    return error;
}
int tcp_lifecycle(NetTcp *tcp) {
    if (!tcp || !tcp->net) return NET_BAD_ARGUMENT;
    if (tcp->state == NET_TCP_FAILED) return tcp->error;
    if (!tcp->active) return NET_CLOSED;
    if (!tcp->net->link || tcp->link_epoch != tcp->net->link_epoch) return tcp_fail(tcp, NET_LINK_DOWN);
    if (tcp->generation != tcp->net->generation) return tcp_fail(tcp, NET_CONFIG_CHANGED);
    return NET_OK;
}
void tcp_flight(NetTcp *tcp, unsigned short flags, unsigned short length) {
    tcp->inflight = 1; tcp->flight_flags = flags; tcp->flight_data = length;
    tcp->flight_seq = tcp->snd_nxt;
    tcp->snd_nxt += length + ((flags & (TCP_SYN | TCP_FIN)) ? 1 : 0);
    tcp->flight_sent = 0; tcp->retransmitted = 0; tcp->sample_valid = 0;
    tcp->retries = 0; tcp->rto = tcp->base_rto;
}
static int local_port_used(NetContext *net, unsigned short port) {
    unsigned short i;
    for (i = 0; i < NET_TCP_CONNECTIONS; ++i) if (net->tcp[i] && net->tcp[i]->local_port == port) return 1;
    return 0;
}
int net_tcp_connect(NetContext *net, NetTcp *tcp, const NetTcpConfig *config, unsigned long now_ms) {
    NetTcpConfig saved;
    unsigned short i, slot = NET_TCP_CONNECTIONS, port, tries;
    if (!net || !tcp || !config || !config->remote_port || !config->peer[0] || config->peer[0] >= 224 || config->peer[0] == 127 ||
        !config->timeout_ms || config->timeout_ms > 600000UL) return NET_BAD_ARGUMENT;
    if (tcp->active || tcp->rx_used) return NET_BUSY;
    if (!net->link) return NET_LINK_DOWN;
    if (!net->config.address[0]) return NET_UNREACHABLE;
    for (i = 0; i < NET_TCP_CONNECTIONS; ++i) if (!net->tcp[i]) { slot = i; break; }
    if (slot == NET_TCP_CONNECTIONS) return NET_NO_PORT;
    port = config->local_port;
    if (port && local_port_used(net, port)) return NET_NO_PORT;
    if (!port) {
        for (tries = 0; tries < NET_TCP_CONNECTIONS + 1; ++tries) {
            port = 49152U + net_random(net) % 16384U;
            if (!local_port_used(net, port)) break;
        }
        if (tries == NET_TCP_CONNECTIONS + 1) return NET_NO_PORT;
    }
    saved = *config; memset(tcp, 0, sizeof(*tcp));
    tcp->net = net; memcpy(tcp->peer, saved.peer, 4);
    tcp->local_port = port; tcp->remote_port = saved.remote_port; tcp->active = 1;
    tcp->state = NET_TCP_SYN_SENT; tcp->mss = 536; tcp->window = 65535U;
    tcp->generation = net->generation; tcp->link_epoch = net->link_epoch;
    tcp->now = now_ms; tcp->last_progress = now_ms; tcp->timeout = saved.timeout_ms;
    tcp->base_rto = 1000; tcp->snd_una = saved.initial_sequence; tcp->snd_nxt = saved.initial_sequence;
    tcp->sent_nxt = saved.initial_sequence;
    net_tcp_enable(net); net->tcp[slot] = tcp; tcp_flight(tcp, TCP_SYN, 0);
    return NET_OK;
}
void net_tcp_abort(NetTcp *tcp) {
    if (!tcp || !tcp->net || !tcp->active) return;
    if (tcp->state != NET_TCP_TIME_WAIT && tcp->state != NET_TCP_SYN_SENT && !tcp->net->pending && !tcp_lifecycle(tcp))
        tcp_send(tcp, tcp->snd_nxt, TCP_RST | TCP_ACK, 0, 0, 0);
    tcp_fail(tcp, NET_CANCELLED);
}
