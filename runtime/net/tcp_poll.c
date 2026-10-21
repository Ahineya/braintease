#include "tcp_internal.h"

static int send_flight(NetTcp *tcp) {
    unsigned short size = tcp->flight_data;
    int status;
    if (size && size > tcp->mss) size = tcp->mss;
    if (size && tcp->window && size > tcp->window) size = tcp->window;
    if (size && !tcp->window) { size = 1; net_count(&tcp->probes); }
    status = tcp_send(tcp, tcp->flight_seq, tcp->flight_flags, NET_TCP_TX_DATA(tcp), size, 1);
    if (status == NET_AGAIN || status == NET_UNREACHABLE) return NET_AGAIN;
    if (status) return tcp_fail(tcp, status);
    if (tcp->flight_flags & TCP_ACK) tcp->ack_pending = 0;
    return NET_OK;
}
static int flight_progress(NetTcp *tcp) {
    if (!tcp->flight_sent) return send_flight(tcp);
    if (tcp->now - tcp->sent_at < tcp->rto) return NET_AGAIN;
    if (tcp->window || !tcp->flight_data) {
        if (++tcp->retries > 7) return tcp_fail(tcp, NET_TIMEOUT);
    }
    if (tcp->send_buffer && tcp->flight_data) {
        tcp->ssthresh = tcp->flight_data / 2;
        if (tcp->ssthresh < tcp->mss * 2) tcp->ssthresh = tcp->mss * 2;
        tcp->cwnd = tcp->mss; tcp->recovering = 0; tcp->duplicate_acks = 0;
    }
    tcp->sample_valid = 0; tcp->retransmitted = 1; tcp->flight_sent = 0;
    if (tcp->rto < 60000UL) tcp->rto = tcp->rto > 30000UL ? 60000UL : tcp->rto * 2;
    return send_flight(tcp);
}
static int fin_window_probe(NetTcp *tcp) {
    int status;
    if (tcp->zero_delay && tcp->now - tcp->zero_at < tcp->zero_delay) return NET_AGAIN;
    status = tcp_send(tcp, tcp->snd_nxt - 1, TCP_ACK, 0, 0, 0);
    if (!status) {
        net_count(&tcp->probes); tcp->zero_at = tcp->now;
        tcp->zero_delay = !tcp->zero_delay ? 1000UL : (tcp->zero_delay > 30000UL ? 60000UL : tcp->zero_delay * 2);
    }
    return status;
}
int net_tcp_poll(NetTcp *tcp, unsigned long now_ms) {
    unsigned short size, burst = 8;
    int status = tcp_lifecycle(tcp);
    if (status) return status;
    tcp->now = now_ms; tcp->net->now = now_ms;
    if (tcp->state == NET_TCP_TIME_WAIT) {
        if (now_ms - tcp->phase_at >= NET_TCP_TIME_WAIT_MS) { tcp_detach(tcp); tcp->state = NET_TCP_CLOSED; return NET_CLOSED; }
    } else if (now_ms - tcp->last_progress >= tcp->timeout) return tcp_fail(tcp, NET_TIMEOUT);
    if (tcp->queued || tcp->net->pending) return NET_AGAIN;
    if (tcp->inflight) {
        status = flight_progress(tcp);
        if (status != NET_AGAIN && status != NET_OK) return status;
        if (tcp->queued) return NET_AGAIN;
    }
    if (!tcp->inflight && (tcp->state == NET_TCP_ESTABLISHED || tcp->state == NET_TCP_CLOSE_WAIT)) {
        if (tcp->tx_used) {
            size = tcp->tx_used;
            if (size > tcp->mss) size = tcp->mss;
            if (tcp->window && size > tcp->window) size = tcp->window;
            if (!tcp->window) size = 1;
            tcp_flight(tcp, TCP_ACK | TCP_PSH, size);
            status = send_flight(tcp); if (status != NET_OK && status != NET_AGAIN) return status;
        } else if (tcp->shutting) {
            if (!tcp->window) {
                status = fin_window_probe(tcp);
                if (status && status != NET_AGAIN && status != NET_UNREACHABLE) return tcp_fail(tcp, status);
                return NET_AGAIN;
            }
            tcp->state = tcp->state == NET_TCP_CLOSE_WAIT ? NET_TCP_LAST_ACK : NET_TCP_FIN_WAIT_1;
            tcp_flight(tcp, TCP_FIN | TCP_ACK, 0);
            status = send_flight(tcp); if (status != NET_OK && status != NET_AGAIN) return status;
        }
    }
    /* Windowed streams append a bounded burst of segments per poll. snd_nxt may
     * include a locally queued segment; sent_nxt only advances on NIC acceptance.
     * Never count that unsent data as remotely acknowledged or restart the
     * oldest outstanding segment's loss timer when extending a flight. */
    while (burst-- && tcp->send_buffer && tcp->inflight && tcp->flight_data && tcp->flight_sent &&
        tcp->sent_nxt == tcp->snd_nxt && !tcp->queued && !tcp->net->pending &&
        !tcp->recovering && tcp->window > tcp->flight_data && tcp->cwnd > tcp->flight_data &&
        tcp->tx_used > tcp->flight_data) {
        size = tcp->tx_used - tcp->flight_data;
        if (size > tcp->mss) size = tcp->mss;
        if (size > tcp->window - tcp->flight_data) size = tcp->window - tcp->flight_data;
        if (size > tcp->cwnd - tcp->flight_data) size = tcp->cwnd - tcp->flight_data;
        status = tcp_send(tcp, tcp->snd_nxt, TCP_ACK | TCP_PSH,
            NET_TCP_TX_DATA(tcp) + tcp->flight_data, size, 1);
        if (!status) { tcp->flight_data += size; tcp->snd_nxt += size; tcp->ack_pending = 0; }
        else if (status != NET_AGAIN && status != NET_UNREACHABLE) return tcp_fail(tcp, status);
        else break;
    }
    if (tcp->ack_pending && !tcp->queued && !tcp->net->pending) {
        status = tcp_send(tcp, tcp->snd_nxt, TCP_ACK, 0, 0, 0);
        if (!status) tcp->ack_pending = 0;
        else if (status != NET_AGAIN && status != NET_UNREACHABLE) return tcp_fail(tcp, status);
    }
    return tcp->state == NET_TCP_ESTABLISHED || tcp->state == NET_TCP_CLOSE_WAIT ? NET_OK : NET_AGAIN;
}
