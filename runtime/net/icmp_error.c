#include "internal.h"

static void notify(NetContext *net, const unsigned char *packet,
                   const unsigned char *ip, const unsigned char *transport) {
    unsigned short i;
    memset(&net->error, 0, sizeof(net->error));
    memcpy(net->error.reporter, net->rx + 26, 4); memcpy(net->error.destination, ip + 16, 4);
    net->error.type = packet[0]; net->error.code = packet[1]; net->error.protocol = ip[9];
    net->error.packet_id = net_read16(ip + 4);
    if (packet[0] == 3 && packet[1] == 4) net->error.mtu = net_read16(packet + 6);
    if (ip[9] == 17 || ip[9] == 6) {
        net->error.source_port = net_read16(transport); net->error.destination_port = net_read16(transport + 2);
    } else {
        net->error.identifier = net_read16(transport + 4); net->error.sequence = net_read16(transport + 6);
    }
    net->event = NET_EVENT_ICMP_ERROR; net_count(&net->counters.icmp_errors);
    if (ip[9] == 17) {
        for (i = 0; i < NET_UDP_PORTS; ++i) {
            if (net->ports[i].port == net->error.source_port && net->ports[i].error_handler) {
                net->ports[i].error_handler(net, &net->error, net->ports[i].user);
                break;
            }
        }
    }
}
int net_icmp_error(NetContext *net, const unsigned char *packet, unsigned short length) {
    const unsigned char *ip = packet + 8, *transport;
    NetSentPacket *sent;
    unsigned short header, total, i;
    if (!net->rx[26] || net->rx[26] >= 224 || net->rx[26] == 127 || length < 36 || ip[0] / 16 != 4) goto malformed;
    header = (ip[0] % 16) * 4; total = net_read16(ip + 2);
    if (header < 20 || header > 60 || length < 8 + header + 8 || total < header + 8 ||
        (net_read16(ip + 6) & 0xbfffU) || net_checksum(ip, header) || memcmp(ip + 12, net->config.address, 4)) goto malformed;
    transport = ip + header;
    for (i = 0; i < NET_TX_HISTORY; ++i) {
        sent = &net->sent[i];
        if (sent->used && net->now - sent->sent >= NET_ERROR_TTL_MS) sent->used = 0;
        if (sent->used && header == 20 && sent->protocol == ip[9] && sent->packet_id == net_read16(ip + 4) &&
            sent->length == total && !memcmp(sent->destination, ip + 16, 4) && !memcmp(sent->prefix, transport, 8)) break;
    }
    if (i == NET_TX_HISTORY) { net_count(&net->counters.icmp_unmatched); return NET_OK; }
    sent->used = 0;
    notify(net, packet, ip, transport);
    return NET_OK;
malformed:
    ++net->rejected; return NET_REJECTED;
}
