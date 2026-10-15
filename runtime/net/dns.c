#include "dns_internal.h"

void net_dns_finish(NetDns *dns, unsigned short status) {
    if (dns->port) net_udp_unbind(dns->net, dns->port);
    dns->port = 0; dns->status = status;
}
void net_dns_cancel(NetDns *dns) {
    if (dns && dns->port) net_dns_finish(dns, NET_DNS_CANCELLED);
}
static int hostname(char *to, const char *from) {
    unsigned short i = 0, label = 0, c;
    if (!from || !*from) return 0;
    while ((c = *from++)) {
        if (c == '.') {
            if (!label || to[i - 1] == '-') return 0;
            if (!*from) break;
            label = 0;
        } else {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) return 0;
            if ((!label && c == '-') || ++label > 63) return 0;
        }
        if (i == 253) return 0;
        to[i++] = c >= 'A' && c <= 'Z' ? c + 32 : c;
    }
    if (!label || to[i - 1] == '-') return 0;
    to[i] = 0; return 1;
}
int net_dns_start(NetContext *net, NetDns *dns, const char *name,
                  unsigned long now_ms, unsigned long timeout_ms, unsigned short retries) {
    unsigned short i, port;
    if (!net || !dns || dns->port || !timeout_ms || timeout_ms > 60000UL || retries > 4 ||
        !net->config.dns[0] || net->config.dns[0] >= 224 || !hostname(dns->name, name)) return NET_BAD_ARGUMENT;
    for (i = 0; i < NET_UDP_PORTS + 1; ++i) {
        port = 49152U + net_random(net) % 16384U;
        if (net_udp_bind(net, port, net_dns_receive, dns) == NET_OK) break;
    }
    if (i == NET_UDP_PORTS + 1) return NET_NO_PORT;
    dns->net = net; dns->port = port; dns->identifier = net_random(net);
    dns->status = NET_DNS_WAITING; dns->attempts = 0; dns->limit = retries + 1;
    dns->sent = 0; dns->malformed = 0; dns->ttl = 0; memset(dns->address, 0, 4);
    dns->started = now_ms; dns->timeout = timeout_ms;
    return NET_OK;
}
int net_dns_poll(NetDns *dns, unsigned long now_ms) {
    unsigned char query[272];
    unsigned short offset, i, start, label;
    int result;
    if (!dns || !dns->port) return dns ? dns->status : NET_DNS_CANCELLED;
    if (now_ms - dns->started >= dns->timeout) {
        if (++dns->attempts == dns->limit) { net_dns_finish(dns, NET_DNS_TIMEOUT); return dns->status; }
        dns->started = now_ms; dns->sent = 0;
        dns->identifier = net_random(dns->net);
    }
    if (dns->sent) return NET_DNS_WAITING;
    memset(query, 0, 12); net_write16(query, dns->identifier);
    net_write16(query + 2, 0x0100); net_write16(query + 4, 1);
    offset = 12; i = 0;
    while (dns->name[i]) {
        start = i; while (dns->name[i] && dns->name[i] != '.') ++i;
        label = i - start; query[offset++] = label;
        memcpy(query + offset, dns->name + start, label); offset += label;
        if (dns->name[i]) ++i;
    }
    query[offset++] = 0; net_write16(query + offset, 1); net_write16(query + offset + 2, 1); offset += 4;
    dns->net->now = now_ms;
    result = net_udp_send(dns->net, dns->net->config.dns, dns->port, 53, query, offset);
    if (result == NET_OK) dns->sent = 1;
    else if (result != NET_AGAIN) net_dns_finish(dns, NET_DNS_NETWORK_ERROR);
    return dns->status;
}
