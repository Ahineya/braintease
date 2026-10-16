#include "tcp_internal.h"

static const NetTcpListenDriver driver = {tcp_passive_input};
static void remove_listener(NetTcpListener *l, int error) {
    unsigned short i;
    for (i = 0; i < NET_TCP_LISTENERS; ++i) if (l->net->listeners[i] == l) l->net->listeners[i] = 0;
    l->active = 0; l->error = error;
    for (i = 0; i < l->capacity; ++i) if (l->pool[i].active && !l->pool[i].accepted) tcp_fail(l->pool + i, error ? error : NET_CANCELLED);
}
static int lifecycle(NetTcpListener *l) {
    if (!l || !l->net) return NET_BAD_ARGUMENT;
    if (!l->active) return l->error ? l->error : NET_CLOSED;
    if (!l->net->link || l->link_epoch != l->net->link_epoch) { remove_listener(l, NET_LINK_DOWN); return NET_LINK_DOWN; }
    if (l->generation != l->net->generation) { remove_listener(l, NET_CONFIG_CHANGED); return NET_CONFIG_CHANGED; }
    return NET_OK;
}
int net_tcp_listen(NetContext *net, NetTcpListener *l, NetTcp *pool, unsigned short capacity, const NetTcpListenConfig *config) {
    unsigned short i, j, k, slot = NET_TCP_LISTENERS;
    NetTcpListener *other;
    if (!net || !l || !pool || !capacity || capacity > NET_TCP_CONNECTIONS || !config || !config->port ||
        !config->sequence || !config->timeout_ms || config->timeout_ms > 600000UL) return NET_BAD_ARGUMENT;
    if (l->active) return NET_BUSY;
    for (i = 0; i < l->capacity; ++i) if (l->pool[i].active || l->pool[i].accepted) return NET_BUSY;
    if (!net->link) return NET_LINK_DOWN;
    if (!net->config.address[0]) return NET_UNREACHABLE;
    for (i = 0; i < capacity; ++i) if (pool[i].active || pool[i].accepted || pool[i].rx_used) return NET_BUSY;
    for (i = 0; i < NET_TCP_CONNECTIONS; ++i) if (net->tcp[i] && net->tcp[i]->local_port == config->port) return NET_NO_PORT;
    for (i = 0; i < NET_TCP_LISTENERS; ++i) {
        other = net->listeners[i];
        if (!other) { if (slot == NET_TCP_LISTENERS) slot = i; continue; }
        if (other->port == config->port) return NET_NO_PORT;
        for (j = 0; j < capacity; ++j) for (k = 0; k < other->capacity; ++k) if (pool + j == other->pool + k) return NET_BUSY;
    }
    if (slot == NET_TCP_LISTENERS) return NET_NO_PORT;
    memset(l, 0, sizeof(*l)); memset(pool, 0, capacity * sizeof(*pool));
    l->net = net; l->pool = pool; l->capacity = capacity; l->port = config->port;
    l->timeout = config->timeout_ms; l->sequence = config->sequence; l->user = config->user;
    l->generation = net->generation; l->link_epoch = net->link_epoch; l->active = 1;
    net->listeners[slot] = l; net->tcp_listen_driver = &driver; net_tcp_enable(net); return NET_OK;
}
void net_tcp_unlisten(NetTcpListener *l) { if (l && l->active) remove_listener(l, 0); }
int net_tcp_listener_poll(NetTcpListener *l, unsigned long now_ms) {
    unsigned short i;
    NetTcp *tcp;
    int status = lifecycle(l);
    if (!l || !l->net) return status;
    for (i = 0; i < l->capacity; ++i) {
        tcp = l->pool + i;
        if (!tcp->active) continue;
        net_tcp_poll(tcp, now_ms);
        if (tcp->active && !tcp->accepted && now_ms - tcp->phase_at >= l->timeout) tcp_fail(tcp, NET_TIMEOUT);
    }
    return status;
}
int net_tcp_accept(NetTcpListener *l, NetTcp **connection) {
    unsigned short i;
    NetTcp *tcp;
    int status;
    if (!connection) return NET_BAD_ARGUMENT;
    *connection = 0; status = lifecycle(l); if (status) return status;
    for (i = 0; i < l->capacity; ++i) {
        tcp = l->pool + i;
        if (tcp->active && !tcp->accepted && !tcp_lifecycle(tcp) && (tcp->state == NET_TCP_ESTABLISHED || tcp->state == NET_TCP_CLOSE_WAIT)) {
            tcp->accepted = 1; *connection = tcp; net_count(&l->accepted); return NET_OK;
        }
    }
    return NET_AGAIN;
}
int net_tcp_release(NetTcpListener *l, NetTcp *tcp) {
    unsigned short i;
    if (!l || !tcp) return NET_BAD_ARGUMENT;
    for (i = 0; i < l->capacity; ++i) if (l->pool + i == tcp) {
        if (!tcp->accepted) return NET_BAD_ARGUMENT;
        if (tcp->active) {
            if (tcp->state != NET_TCP_TIME_WAIT || tcp_wait_save(tcp)) return NET_BUSY;
        }
        memset(tcp, 0, sizeof(*tcp)); return NET_OK;
    }
    return NET_BAD_ARGUMENT;
}
int tcp_passive_input(NetContext *net, const TcpSegment *s) {
    unsigned short i, j, slot = NET_TCP_CONNECTIONS;
    NetTcpListener *l;
    NetTcp *tcp = 0;
    if (!(s->flags & TCP_SYN) || (s->flags & (TCP_ACK | TCP_RST))) return 0;
    for (i = 0; i < NET_TCP_LISTENERS; ++i) {
        l = net->listeners[i]; if (!l || l->port != s->destination_port || lifecycle(l)) continue;
        if (s->length || (s->flags & TCP_URG)) { net_count(&l->refused); return 1; }
        for (j = 0; j < l->capacity; ++j) if (!l->pool[j].active && !l->pool[j].accepted) { tcp = l->pool + j; break; }
        for (j = 0; j < NET_TCP_CONNECTIONS; ++j) if (!net->tcp[j]) { slot = j; break; }
        if (!tcp || slot == NET_TCP_CONNECTIONS) { net_count(&l->refused); return 1; }
        memset(tcp, 0, sizeof(*tcp)); tcp->net = net; tcp->active = 1; tcp->state = NET_TCP_SYN_RECEIVED;
        memcpy(tcp->peer, s->source, 4); tcp->local_port = l->port; tcp->remote_port = s->source_port;
        tcp->mss = s->mss < NET_TCP_MSS ? s->mss : NET_TCP_MSS; tcp->window = s->window;
        tcp->generation = net->generation; tcp->link_epoch = net->link_epoch;
        tcp->now = net->now; tcp->last_progress = net->now; tcp->phase_at = net->now; tcp->timeout = l->timeout; tcp->base_rto = 1000;
        tcp->snd_una = l->sequence(l->user, s->source, s->source_port, net->now); tcp->snd_nxt = tcp->snd_una; tcp->sent_nxt = tcp->snd_nxt;
        tcp->rcv_nxt = s->sequence + 1; tcp->window_seq = s->sequence;
        net->tcp[slot] = tcp; tcp_flight(tcp, TCP_SYN | TCP_ACK, 0); return 1;
    }
    return 0;
}
