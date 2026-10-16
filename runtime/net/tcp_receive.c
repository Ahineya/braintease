#include "tcp_internal.h"

void tcp_challenge(NetTcp *tcp) {
    if (!tcp->challenge_sent || tcp->now - tcp->challenge_at >= 1000UL) {
        tcp->ack_pending = 1; tcp->challenge_sent = 1; tcp->challenge_at = tcp->now;
    }
}
void tcp_established(NetTcp *tcp, const TcpSegment *segment) {
    tcp_rtt(tcp);
    if (tcp->retransmitted && tcp->base_rto < 3000UL) tcp->base_rto = 3000UL;
    tcp->snd_una = tcp->snd_nxt;
    tcp->inflight = 0; tcp->state = NET_TCP_ESTABLISHED; tcp->last_progress = tcp->now;
    tcp->window = segment->window; tcp->window_seq = segment->sequence; tcp->window_ack = segment->acknowledgement;
}
static void syn_reply(NetTcp *tcp, const TcpSegment *segment) {
    TcpSegment data;
    if (!(segment->flags & TCP_ACK) || segment->acknowledgement != tcp->snd_nxt || tcp->sent_nxt != tcp->snd_nxt) return;
    if (segment->flags & TCP_RST) { tcp_fail(tcp, NET_RESET); return; }
    if (!(segment->flags & TCP_SYN)) return;
    if (segment->flags & TCP_URG) { tcp_fail(tcp, NET_UNSUPPORTED); return; }
    tcp->rcv_nxt = segment->sequence + 1; tcp_established(tcp, segment);
    tcp->mss = segment->mss < NET_TCP_MSS ? segment->mss : NET_TCP_MSS;
    tcp->ack_pending = 1;
    if (segment->length) {
        data = *segment; ++data.sequence; data.flags &= 0xfffdU;
        tcp_payload(tcp, &data);
    }
}
int tcp_acceptable(NetTcp *tcp, const TcpSegment *segment) {
    unsigned short window = NET_TCP_BUFFER - tcp->rx_used;
    unsigned long space = segment->length + ((segment->flags & (TCP_SYN | TCP_FIN)) ? 1 : 0);
    if (!space) return segment->sequence == tcp->rcv_nxt || (window && segment->sequence - tcp->rcv_nxt < window);
    if (!window) return 0;
    if (tcp_before(segment->sequence, tcp->rcv_nxt)) return tcp->rcv_nxt - segment->sequence < space;
    return segment->sequence - tcp->rcv_nxt < window;
}
void tcp_receive(NetTcp *tcp, const TcpSegment *segment) {
    if (tcp->state == NET_TCP_SYN_SENT) { syn_reply(tcp, segment); return; }
    if (tcp->state == NET_TCP_SYN_RECEIVED) { tcp_passive_receive(tcp, segment); return; }
    if (tcp->state == NET_TCP_TIME_WAIT) {
        if ((segment->flags & TCP_FIN) && !(segment->flags & TCP_RST) && segment->sequence + segment->length + 1 == tcp->rcv_nxt) {
            tcp->ack_pending = 1; tcp->phase_at = tcp->now;
        }
        return;
    }
    if (segment->flags & TCP_RST) {
        if (segment->sequence == tcp->rcv_nxt) tcp_fail(tcp, NET_RESET);
        else if (segment->sequence - tcp->rcv_nxt < NET_TCP_BUFFER - tcp->rx_used) tcp_challenge(tcp);
        return;
    }
    if (segment->flags & TCP_SYN) {
        if ((segment->flags & TCP_ACK) && segment->sequence + segment->length + 1 == tcp->rcv_nxt && segment->acknowledgement == tcp->snd_una) tcp->ack_pending = 1;
        else tcp_challenge(tcp);
        return;
    }
    if (!tcp_acceptable(tcp, segment)) { tcp->ack_pending = 1; return; }
    if (!(segment->flags & TCP_ACK)) return;
    if (!tcp_acknowledge(tcp, segment) || !tcp->active) return;
    if (segment->flags & TCP_URG) { net_tcp_abort(tcp); tcp->error = NET_UNSUPPORTED; return; }
    tcp_payload(tcp, segment);
}
