#include "tcp_internal.h"

void tcp_passive_receive(NetTcp *tcp, const TcpSegment *s) {
    if (s->flags & TCP_RST) {
        if (s->sequence == tcp->rcv_nxt) tcp_fail(tcp, NET_RESET);
        else if (s->sequence - tcp->rcv_nxt < NET_TCP_RX_CAP(tcp)) tcp_challenge(tcp);
        return;
    }
    if (s->flags & TCP_SYN) {
        if (!(s->flags & (TCP_ACK | TCP_URG)) && s->sequence + 1 == tcp->rcv_nxt) {
            /* Lost SYN-ACK: retransmit the same ISN without extending the
             * handshake's progress deadline or measuring an ambiguous RTT. */
            tcp->flight_sent = 0; tcp->retransmitted = 1; tcp->sample_valid = 0;
        } else tcp_challenge(tcp);
        return;
    }
    if (!tcp_acceptable(tcp, s)) { tcp_challenge(tcp); return; }
    if (!(s->flags & TCP_ACK)) return;
    if (s->acknowledgement != tcp->snd_nxt || tcp->sent_nxt != tcp->snd_nxt) { tcp_reset_segment(tcp->net, s); return; }
    if (s->flags & TCP_URG) { net_tcp_abort(tcp); tcp->error = NET_UNSUPPORTED; return; }
    tcp_established(tcp, s); tcp_payload(tcp, s);
}
