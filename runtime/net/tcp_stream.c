#include "tcp_internal.h"

int net_tcp_write(NetTcp *tcp, const unsigned char *data, unsigned short length, unsigned short *written) {
    unsigned short size;
    int status;
    if (!written || (length && !data)) return NET_BAD_ARGUMENT;
    *written = 0; status = tcp_lifecycle(tcp); if (status) return status;
    if (tcp->state == NET_TCP_SYN_SENT || tcp->state == NET_TCP_SYN_RECEIVED) return NET_AGAIN;
    if ((tcp->state != NET_TCP_ESTABLISHED && tcp->state != NET_TCP_CLOSE_WAIT) || tcp->shutting) return NET_CLOSED;
    size = NET_TCP_BUFFER - tcp->tx_used; if (size > length) size = length;
    if (size) memcpy(tcp->tx + tcp->tx_used, data, size);
    tcp->tx_used += size; tcp->bytes_written += size; *written = size;
    return size || !length ? NET_OK : NET_AGAIN;
}
int net_tcp_read(NetTcp *tcp, unsigned char *data, unsigned short capacity, unsigned short *received) {
    unsigned short size, i;
    int status;
    if (!received || (capacity && !data)) return NET_BAD_ARGUMENT;
    *received = 0; status = tcp_lifecycle(tcp);
    if (status && !(status == NET_CLOSED && tcp && tcp->peer_fin)) return status;
    if (!capacity) return NET_OK;
    size = tcp->rx_used; if (size > capacity) size = capacity;
    for (i = 0; i < size; ++i) data[i] = tcp->rx[(tcp->rx_head + i) % NET_TCP_BUFFER];
    tcp->rx_head = (tcp->rx_head + size) % NET_TCP_BUFFER; tcp->rx_used -= size;
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
