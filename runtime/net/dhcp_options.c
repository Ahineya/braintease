#include "dhcp_internal.h"

static int options(const unsigned char *p, unsigned short length, DhcpReply *reply, int main) {
    unsigned short offset = 0, code, size, bit;
    const unsigned char *value;
    while (offset < length) {
        code = p[offset++];
        if (!code) continue;
        if (code == 255) return 1;
        if (offset == length) return 0;
        size = p[offset++];
        if (size > length - offset) return 0;
        value = p + offset; offset += size; bit = 0;
        if (code == 1) bit = DHCP_MASK;
        if (code == 3) bit = DHCP_ROUTER;
        if (code == 6) bit = DHCP_DNS;
        if (code == 51) bit = DHCP_LEASE;
        if (code == 54) bit = DHCP_SERVER;
        if (code == 53) bit = DHCP_TYPE;
        if (code == 58) bit = DHCP_T1;
        if (code == 59) bit = DHCP_T2;
        if (code == 52) bit = DHCP_OVERLOAD;
        if (!bit) continue;
        /* Deliberately reject repeated critical options; no RFC3396 joining. */
        if (reply->seen & bit) return 0;
        reply->seen |= bit;
        if (bit == DHCP_TYPE || bit == DHCP_OVERLOAD) {
            if (size != 1) return 0;
            if (bit == DHCP_TYPE) reply->type = value[0];
            else { if (!main || !value[0] || value[0] > 3) return 0; reply->overload = value[0]; }
        } else if (bit == DHCP_ROUTER || bit == DHCP_DNS) {
            if (!size || size % 4 || !dhcp_ip(value)) return 0;
            if (bit == DHCP_ROUTER) memcpy(reply->config.gateway, value, 4);
            else memcpy(reply->config.dns, value, 4);
        } else {
            if (size != 4) return 0;
            if (bit == DHCP_MASK) memcpy(reply->config.mask, value, 4);
            if (bit == DHCP_SERVER) { if (!dhcp_ip(value)) return 0; memcpy(reply->server, value, 4); }
            if (bit == DHCP_LEASE) reply->lease = dhcp_read32(value);
            if (bit == DHCP_T1) reply->t1 = dhcp_read32(value);
            if (bit == DHCP_T2) reply->t2 = dhcp_read32(value);
        }
    }
    return 0;
}
int dhcp_decode(const unsigned char *p, unsigned short length, DhcpReply *reply) {
    if (length < 240 || p[0] != 2 || p[1] != 1 || p[2] != 6 || p[3] > 16 ||
        p[236] != 99 || p[237] != 130 || p[238] != 83 || p[239] != 99) return 0;
    memset(reply, 0, sizeof(*reply));
    memcpy(reply->config.address, p + 16, 4);
    if (!options(p + 240, length - 240, reply, 1)) return 0;
    if ((reply->overload & 1) && !options(p + 108, 128, reply, 0)) return 0;
    if ((reply->overload & 2) && !options(p + 44, 64, reply, 0)) return 0;
    return (reply->seen & (DHCP_TYPE | DHCP_SERVER)) == (DHCP_TYPE | DHCP_SERVER);
}
