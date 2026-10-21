#include "tcp_internal.h"

static void received_fin(NetTcp *tcp) {
    ++tcp->rcv_nxt; tcp->peer_fin = 1; tcp->ack_pending = 1; tcp->last_progress = tcp->now;
    if (tcp->state == NET_TCP_ESTABLISHED) tcp->state = NET_TCP_CLOSE_WAIT;
    else if (tcp->state == NET_TCP_FIN_WAIT_1) tcp->state = NET_TCP_CLOSING;
    else if (tcp->state == NET_TCP_FIN_WAIT_2) tcp_time_wait(tcp);
}
void tcp_payload(NetTcp *tcp, const TcpSegment *segment) {
    unsigned long sequence = segment->sequence, skip = 0;
    unsigned short size = segment->length, available, tail, first;
    if (!size && !(segment->flags & TCP_FIN)) return;
    tcp->ack_pending = 1;
    if (tcp->peer_fin) return;
    if (tcp_before(sequence, tcp->rcv_nxt)) {
        skip = tcp->rcv_nxt - sequence;
        if (skip > size) return;
        sequence += skip; size -= skip;
    }
    available = NET_TCP_RX_CAP(tcp) - tcp->rx_used;
    if (tcp->receive_map && (sequence != tcp->rcv_nxt || tcp->reorder_used || tcp->pending_fin)) {
        unsigned long offset = sequence - tcp->rcv_nxt;
        unsigned short at, i, bit, accepted = 0;
        if (offset >= available) return;
        if (size > available - offset) size = available - offset;
        tail = (tcp->rx_head + tcp->rx_used) % NET_TCP_RX_CAP(tcp);
        at = (tail + (unsigned short)offset) % NET_TCP_RX_CAP(tcp);
        for (i = 0; i < size; ++i) {
            bit = 1U << (at & 7);
            if (!(tcp->receive_map[at / 8] & bit)) {
                NET_TCP_RX_DATA(tcp)[at] = segment->data[skip + i];
                tcp->receive_map[at / 8] |= bit; ++tcp->reorder_used;
            }
            if (++at == NET_TCP_RX_CAP(tcp)) at = 0;
        }
        if ((segment->flags & TCP_FIN) && offset + size < available &&
            skip + size == segment->length && !tcp->pending_fin) {
            tcp->pending_fin = 1; tcp->fin_sequence = segment->sequence + segment->length;
        }
        while (accepted < available && (!tcp->pending_fin || tcp->rcv_nxt + accepted != tcp->fin_sequence) && (tcp->receive_map[tail / 8] & (1U << (tail & 7)))) {
            tcp->receive_map[tail / 8] &= ~(1U << (tail & 7));
            --tcp->reorder_used; ++accepted;
            if (++tail == NET_TCP_RX_CAP(tcp)) tail = 0;
        }
        tcp->rx_used += accepted; tcp->rcv_nxt += accepted; tcp->bytes_received += accepted;
        if (accepted) tcp->last_progress = tcp->now;
        if (tcp->pending_fin && tcp->fin_sequence == tcp->rcv_nxt && accepted < available) {
            tcp->pending_fin = 0; tcp->reorder_used = 0; received_fin(tcp);
        }
        return;
    }
    if (sequence != tcp->rcv_nxt) return;
    available = NET_TCP_RX_CAP(tcp) - tcp->rx_used;
    if (size > available) size = available;
    tail = (tcp->rx_head + tcp->rx_used) % NET_TCP_RX_CAP(tcp);
    first = NET_TCP_RX_CAP(tcp) - tail; if (first > size) first = size;
    if (first) memcpy(NET_TCP_RX_DATA(tcp) + tail, segment->data + skip, first);
    if (size > first) memcpy(NET_TCP_RX_DATA(tcp), segment->data + skip + first, size - first);
    tcp->rx_used += size; tcp->rcv_nxt += size; tcp->bytes_received += size;
    if (size) tcp->last_progress = tcp->now;
    if ((segment->flags & TCP_FIN) && segment->sequence + segment->length == tcp->rcv_nxt && size < available) received_fin(tcp);
}
