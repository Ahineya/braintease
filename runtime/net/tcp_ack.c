#include "tcp_internal.h"

void tcp_rtt(NetTcp *tcp) {
    unsigned long sample, difference, value;
    if (!tcp->sample_valid || tcp->retransmitted) return;
    tcp->sample_valid = 0; sample = tcp->now - tcp->sample_at;
    if (!sample) sample = 1;
    if (!tcp->srtt) { tcp->srtt = sample; tcp->rttvar = sample / 2; }
    else {
        difference = tcp->srtt > sample ? tcp->srtt - sample : sample - tcp->srtt;
        tcp->rttvar = (3UL * tcp->rttvar + difference) / 4;
        tcp->srtt = (7UL * tcp->srtt + sample) / 8;
    }
    value = tcp->srtt + 4UL * tcp->rttvar;
    if (value < 1000UL) value = 1000;
    if (value > 60000UL) value = 60000;
    tcp->base_rto = value;
}
void tcp_time_wait(NetTcp *tcp) {
    tcp->state = NET_TCP_TIME_WAIT; tcp->phase_at = tcp->now;
}
static void fin_ack(NetTcp *tcp) {
    if (tcp->state == NET_TCP_FIN_WAIT_1) tcp->state = NET_TCP_FIN_WAIT_2;
    else if (tcp->state == NET_TCP_CLOSING) tcp_time_wait(tcp);
    else if (tcp->state == NET_TCP_LAST_ACK) { tcp_detach(tcp); tcp->state = NET_TCP_CLOSED; }
}
int tcp_acknowledge(NetTcp *tcp, const TcpSegment *segment) {
    unsigned long ack = segment->acknowledgement, delta = ack - tcp->snd_una;
    unsigned short size, i, old_window = tcp->window;
    if (delta > tcp->sent_nxt - tcp->snd_una) {
        if (!tcp_before(ack, tcp->snd_una)) { tcp_challenge(tcp); return 0; }
        return 1; /* An older ACK may still accompany new, acceptable data. */
    }
    if (tcp_before(tcp->window_seq, segment->sequence) ||
        (tcp->window_seq == segment->sequence && !tcp_before(ack, tcp->window_ack))) {
        tcp->window = segment->window; tcp->window_seq = segment->sequence; tcp->window_ack = ack;
    }
    if (!delta && !old_window && tcp->window && tcp->inflight && tcp->flight_data) {
        tcp->flight_sent = 0; tcp->retransmitted = 1; tcp->sample_valid = 0;
    }
    if (!delta) return 1;
    tcp_rtt(tcp); tcp->snd_una = ack; tcp->last_progress = tcp->now; tcp->retries = 0;
    tcp->sent_at = tcp->now;
    if (tcp->flight_flags & TCP_FIN) {
        tcp->inflight = 0; fin_ack(tcp); return 1;
    }
    size = delta;
    for (i = size; i < tcp->tx_used; ++i) tcp->tx[i - size] = tcp->tx[i];
    tcp->tx_used -= size; tcp->flight_data -= size; tcp->flight_seq += size; tcp->bytes_acked += size;
    if (!tcp->flight_data) tcp->inflight = 0;
    else if (tcp->snd_una == tcp->sent_nxt) {
        /* The window shrank before the original local send was accepted.
         * Send the still-unsent tail without waiting for a remote-loss timer. */
        tcp->flight_sent = 0; tcp->retransmitted = 1;
    }
    return 1;
}
