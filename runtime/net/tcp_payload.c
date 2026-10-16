#include "tcp_internal.h"

static void received_fin(NetTcp *tcp) {
    ++tcp->rcv_nxt; tcp->peer_fin = 1; tcp->ack_pending = 1; tcp->last_progress = tcp->now;
    if (tcp->state == NET_TCP_ESTABLISHED) tcp->state = NET_TCP_CLOSE_WAIT;
    else if (tcp->state == NET_TCP_FIN_WAIT_1) tcp->state = NET_TCP_CLOSING;
    else if (tcp->state == NET_TCP_FIN_WAIT_2) tcp_time_wait(tcp);
}
void tcp_payload(NetTcp *tcp, const TcpSegment *segment) {
    unsigned long sequence = segment->sequence, skip = 0;
    unsigned short size = segment->length, available, i;
    if (!size && !(segment->flags & TCP_FIN)) return;
    tcp->ack_pending = 1;
    if (tcp->peer_fin) return;
    if (tcp_before(sequence, tcp->rcv_nxt)) {
        skip = tcp->rcv_nxt - sequence;
        if (skip > size) return;
        sequence += skip; size -= skip;
    }
    if (sequence != tcp->rcv_nxt) return; /* Bounded memory: duplicate ACK, no out-of-order queue. */
    available = NET_TCP_BUFFER - tcp->rx_used;
    if (size > available) size = available;
    for (i = 0; i < size; ++i) tcp->rx[(tcp->rx_head + tcp->rx_used + i) % NET_TCP_BUFFER] = segment->data[skip + i];
    tcp->rx_used += size; tcp->rcv_nxt += size; tcp->bytes_received += size;
    if (size) tcp->last_progress = tcp->now;
    if ((segment->flags & TCP_FIN) && segment->sequence + segment->length == tcp->rcv_nxt && size < available) received_fin(tcp);
}
