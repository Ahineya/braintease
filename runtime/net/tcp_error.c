#include "tcp_internal.h"

void net_tcp_error(NetContext *net, const NetError *error) {
    NetTcp *tcp;
    unsigned short i;
    for (i = 0; i < NET_TCP_CONNECTIONS; ++i) {
        tcp = net->tcp[i];
        if (!tcp || tcp->local_port != error->source_port || tcp->remote_port != error->destination_port || memcmp(tcp->peer, error->destination, 4)) continue;
        if (tcp_lifecycle(tcp)) return;
        tcp->icmp_type = error->type; tcp->icmp_code = error->code; tcp->mtu = error->mtu;
        if (error->type == 3 && (error->code == 2 || error->code == 3)) tcp_fail(tcp, NET_UNREACHABLE);
        else if (error->type == 12 || (error->type == 3 && error->code == 4)) tcp_fail(tcp, NET_PATH_ERROR);
        /* Network/host unreachable and TTL errors are soft: keep retrying
         * within the configured progress deadline rather than killing a stream. */
        return;
    }
}
