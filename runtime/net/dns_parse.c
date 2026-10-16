#include "dns_internal.h"

static unsigned long read32(const unsigned char *p) {
    return (unsigned long)net_read16(p) * 65536UL + net_read16(p + 2);
}
/* Validate every record before accepting any address. A CNAME chain may appear
 * in any answer order, but must lead from our exact question to an A record. */
static int answers(NetDns *dns, const unsigned char *p, unsigned short length, unsigned short offset,
                    unsigned short answer_count, unsigned short count) {
    unsigned short records[48], n, next, type, klass, size, data, hop, found;
    unsigned long ttl = 0xffffffffUL, value;
    char owner[254], target[254];
    for (n = 0; n < count; ++n) {
        records[n] = offset;
        if (!net_dns_name(p, length, offset, owner, &next) || length - next < 10) return 0;
        type = net_read16(p + next); klass = net_read16(p + next + 2);
        size = net_read16(p + next + 8); data = next + 10;
        if (size > length - data) return 0;
        if (klass == 1 && type == 1 && size != 4) return 0;
        if (klass == 1 && type == 5 && (!net_dns_name(p, length, data, target, &next) || next != data + size)) return 0;
        offset = data + size;
    }
    if (offset != length) return 0;
    strcpy(target, dns->name);
    for (hop = 0; hop < 9; ++hop) {
        found = 0;
        for (n = 0; n < answer_count; ++n) {
            if (!net_dns_name(p, length, records[n], owner, &next)) return 0;
            if (strcmp(owner, target) || net_read16(p + next + 2) != 1) continue;
            type = net_read16(p + next); data = next + 10;
            value = read32(p + next + 4);
            if (type == 1) {
                memcpy(dns->address, p + data, 4); dns->ttl = value < ttl ? value : ttl;
                net_dns_finish(dns, NET_DNS_SUCCESS); return 1;
            }
            if (type == 5 && !found) {
                if (hop == 8 || !net_dns_name(p, length, data, owner, &next)) return 0;
                /* Save until the end of this scan so order cannot affect it. */
                found = data;
                if (value < ttl) ttl = value;
            }
        }
        if (!found) { net_dns_finish(dns, NET_DNS_NO_ADDRESS); return 1; }
        if (!net_dns_name(p, length, found, target, &next)) return 0;
    }
    return 0;
}
void net_dns_receive(NetContext *net, const NetDatagram *packet, void *user) {
    NetDns *dns = user;
    const unsigned char *p = packet->data;
    char question[254];
    unsigned short length = packet->length, flags, next, an, ns, ar;
    if (!net->link || dns->link_epoch != net->link_epoch) { net_dns_finish(dns, NET_DNS_LINK_DOWN); return; }
    if (dns->generation != net->generation) { net_dns_finish(dns, NET_DNS_CONFIG_CHANGED); return; }
    if (!dns->sent || packet->source_port != 53 || memcmp(packet->source, net->config.dns, 4) ||
        length < 12 || net_read16(p) != dns->identifier) return;
    flags = net_read16(p + 2);
    if ((flags & 0xf840U) != 0x8000U || net_read16(p + 4) != 1) { ++dns->malformed; return; }
    if (length > 512 || !net_dns_name(p, length, 12, question, &next) || length - next < 4) { ++dns->malformed; return; }
    if (strcmp(question, dns->name) || net_read16(p + next) != 1 || net_read16(p + next + 2) != 1) return;
    if (flags & 0x0200U) { net_dns_finish(dns, NET_DNS_TRUNCATED); return; }
    an = net_read16(p + 6); ns = net_read16(p + 8); ar = net_read16(p + 10);
    if (an > 48 || ns > 48 || ar > 48 || an + ns + ar > 48) { ++dns->malformed; return; }
    /* Errors still need well-formed sections, but cannot supply an address. */
    if (!answers(dns, p, length, next + 4, (flags & 15) ? 0 : an, an + ns + ar)) { ++dns->malformed; return; }
    if (flags & 15) net_dns_finish(dns, (flags & 15) == 3 ? NET_DNS_NXDOMAIN : NET_DNS_SERVER_ERROR);
}
