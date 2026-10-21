#include "tcp_internal.h"

int net_tcp_write(NetTcp *tcp, const unsigned char *data, unsigned short length, unsigned short *written) {
    unsigned short size;
    int status;
    if (!written || (length && !data)) return NET_BAD_ARGUMENT;
    *written = 0; status = tcp_lifecycle(tcp); if (status) return status;
    if (tcp->state == NET_TCP_SYN_SENT || tcp->state == NET_TCP_SYN_RECEIVED) return NET_AGAIN;
    if ((tcp->state != NET_TCP_ESTABLISHED && tcp->state != NET_TCP_CLOSE_WAIT) || tcp->shutting) return NET_CLOSED;
    size = NET_TCP_TX_CAP(tcp) - tcp->tx_used; if (size > length) size = length;
    if (size) memcpy(NET_TCP_TX_DATA(tcp) + tcp->tx_used, data, size);
    tcp->tx_used += size; tcp->bytes_written += size; *written = size;
    return size || !length ? NET_OK : NET_AGAIN;
}
int net_tcp_read(NetTcp *tcp, unsigned char *data, unsigned short capacity, unsigned short *received) {
    unsigned short size, first;
    int status;
    if (!received || (capacity && !data)) return NET_BAD_ARGUMENT;
    *received = 0; status = tcp_lifecycle(tcp);
    if (status && !(status == NET_CLOSED && tcp && tcp->peer_fin)) return status;
    if (!capacity) return NET_OK;
    size = tcp->rx_used; if (size > capacity) size = capacity;
    first = NET_TCP_RX_CAP(tcp) - tcp->rx_head; if (first > size) first = size;
    if (first) memcpy(data, NET_TCP_RX_DATA(tcp) + tcp->rx_head, first);
    if (size > first) memcpy(data + first, NET_TCP_RX_DATA(tcp), size - first);
    tcp->rx_head = (tcp->rx_head + size) % NET_TCP_RX_CAP(tcp); tcp->rx_used -= size;
    tcp->bytes_read += size; *received = size;
    if (size) { if (tcp->active) tcp->ack_pending = 1; return NET_OK; }
    return tcp->peer_fin ? NET_EOF : NET_AGAIN;
}
int net_tcp_shutdown(NetTcp *tcp) {
    int status = tcp_lifecycle(tcp);
    if (status) return status;
    if (tcp->shutting) return NET_OK;
    if (tcp->state != NET_TCP_ESTABLISHED && tcp->state != NET_TCP_CLOSE_WAIT) return NET_AGAIN;
    tcp->shutting = 1; return NET_OK;
}

int net_tcp_buffers(NetTcp *tcp, unsigned char *send, unsigned short send_capacity,
    unsigned char *receive, unsigned short receive_capacity, unsigned char *map) {
    unsigned short first;
    if (!tcp || !send || !receive || !map || send_capacity < NET_TCP_BUFFER ||
        send_capacity > 16384 || receive_capacity < NET_TCP_RX_BUFFER ||
        receive_capacity > 32768U) return NET_BAD_ARGUMENT;
    if (!tcp->active || tcp->send_buffer || tcp->receive_buffer || tcp->tx_used ||
        tcp->queued || (tcp->state != NET_TCP_SYN_SENT && tcp->state != NET_TCP_ESTABLISHED &&
        tcp->state != NET_TCP_CLOSE_WAIT)) return NET_BUSY;
    if (tcp->rx_used > receive_capacity) return NET_BUSY;
    first = NET_TCP_RX_BUFFER - tcp->rx_head;
    if (first > tcp->rx_used) first = tcp->rx_used;
    if (first) memcpy(receive, tcp->rx + tcp->rx_head, first);
    if (tcp->rx_used > first) memcpy(receive + first, tcp->rx, tcp->rx_used - first);
    memset(map, 0, (receive_capacity + 7U) / 8U);
    tcp->send_buffer = send; tcp->send_capacity = send_capacity;
    tcp->receive_buffer = receive; tcp->receive_capacity = receive_capacity;
    tcp->receive_map = map; tcp->rx_head = 0;
    tcp->cwnd = tcp->mss * 2; tcp->ssthresh = send_capacity;
    /* The SYN carries the new window. An ACK before SYN-ACK would use an
     * uninitialized receive sequence and can reset a real peer. */
    tcp->ack_pending = tcp->state != NET_TCP_SYN_SENT;
    return NET_OK;
}
