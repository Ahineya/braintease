#include "tcp_internal.h"

static NetTcpWaitTable *table(NetContext *net) {
    NetTcpWaitTable *t = net->tcp_wait;
    if (t && (t->generation != net->generation || t->link_epoch != net->link_epoch)) {
        memset(t->entries, 0, sizeof(t->entries)); t->used = 0;
        t->generation = net->generation; t->link_epoch = net->link_epoch;
    }
    return t;
}
static int live(NetContext *net, NetTcpWait *wait) {
    if (wait->used && net->now - wait->started >= NET_TCP_TIME_WAIT_MS) { wait->used = 0; --net->tcp_wait->used; }
    return wait->used;
}
int tcp_wait_port(NetContext *net, unsigned short port) {
    unsigned short i;
    NetTcpWaitTable *t = table(net);
    if (!t || !t->used) return 0;
    for (i = 0; i < NET_TCP_WAIT_SLOTS; ++i) if (live(net, t->entries + i) && t->entries[i].local_port == port) return 1;
    return 0;
}
int tcp_wait_save(NetTcp *tcp) {
    NetTcpWait *wait;
    NetTcpWaitTable *t = table(tcp->net);
    unsigned short i;
    if (!t) return NET_BUSY;
    /* Keep a valid old tuple only in the configuration in which it existed. */
    if (tcp_lifecycle(tcp)) return NET_CLOSED;
    for (i = 0; i < NET_TCP_WAIT_SLOTS; ++i) {
        wait = t->entries + i; if (live(tcp->net, wait)) continue;
        memcpy(wait->peer, tcp->peer, 4); wait->local_port = tcp->local_port; wait->remote_port = tcp->remote_port;
        wait->snd_nxt = tcp->snd_nxt; wait->rcv_nxt = tcp->rcv_nxt; wait->started = tcp->phase_at; wait->used = 1; ++t->used;
        tcp_detach(tcp); tcp->state = NET_TCP_CLOSED; return NET_OK;
    }
    return NET_BUSY;
}
int net_tcp_retire(NetTcp *tcp) {
    int status;
    if (!tcp || !tcp->net) return NET_BAD_ARGUMENT;
    if (!tcp->active) return NET_OK;
    if (tcp->state != NET_TCP_TIME_WAIT) return NET_BUSY;
    status = tcp_wait_save(tcp);
    return tcp->active ? status : NET_OK;
}
int tcp_wait_input(NetContext *net, const TcpSegment *s) {
    unsigned short i;
    unsigned char *p;
    NetTcpWait *wait;
    NetTcpWaitTable *t = table(net);
    if (!t || !t->used) return 0;
    for (i = 0; i < NET_TCP_WAIT_SLOTS; ++i) {
        wait = t->entries + i;
        if (!live(net, wait) || wait->local_port != s->destination_port || wait->remote_port != s->source_port || memcmp(wait->peer, s->source, 4)) continue;
        if ((s->flags & TCP_FIN) && !(s->flags & TCP_RST) && s->sequence + s->length + 1 == wait->rcv_nxt) {
            wait->started = net->now;
            /* The peer still needs its last ACK. Use the received L2 next hop;
             * no application data or mutable TCB has to be resurrected. */
            net_ethernet(net, net->rx + 6, 0x0800); net_ipv4_header(net, s->source, 6, 20);
            p = net->tx + 34; memset(p, 0, 20);
            net_write16(p, wait->local_port); net_write16(p + 2, wait->remote_port);
            tcp_write32(p + 4, wait->snd_nxt); tcp_write32(p + 8, wait->rcv_nxt); p[12] = 80; p[13] = TCP_ACK;
            net_write16(p + 16, net_l4_checksum(net->config.address, s->source, 6, p, 20)); net_transmit(net, 54);
        }
        return 1; /* Delayed data, SYN and RST cannot create a new connection. */
    }
    return 0;
}

int net_tcp_wait_attach(NetContext *net, NetTcpWaitTable *next) {
    NetTcpWaitTable *old;
    unsigned short i;
    if (!net) return NET_BAD_ARGUMENT;
    old = table(net);
    if (old == next) return NET_OK;
    if (old) {
        for (i = 0; i < NET_TCP_WAIT_SLOTS; ++i) live(net, old->entries + i);
        if (old->used) return NET_BUSY;
    }
    if (next && (next->net || next->used)) return NET_BUSY;
    if (old) old->net = 0;
    net->tcp_wait = next;
    if (next) { next->net = net; next->generation = net->generation; next->link_epoch = net->link_epoch; }
    return NET_OK;
}
