#include <net_dns.h>
#include <stdio.h>
#include <string.h>
/* Internal decoder tested directly at every boundary; transport/reply matching
 * is independently exercised by scripts/test-net-dns.py. */
int net_dns_name(const unsigned char *, unsigned short, unsigned short, char *, unsigned short *);
static unsigned char packet[512];
static char result[254];
static NetContext net;
static NetDns dns;
static void check(int condition) { putchar(condition ? 'Y' : 'N'); }
int main(void) {
    unsigned short next = 0, i, offset;
    memset(packet, 0, sizeof(packet));
    packet[12] = 3; packet[13] = 'W'; packet[14] = 'W'; packet[15] = 'W';
    packet[16] = 7; memcpy(packet + 17, "Example", 7); packet[24] = 0;
    check(net_dns_name(packet, 25, 12, result, &next) && !strcmp(result, "www.example") && next == 25);
    packet[25] = 192; packet[26] = 12;
    check(net_dns_name(packet, 27, 25, result, &next) && next == 27 && !strcmp(result, "www.example"));
    check(!net_dns_name(packet, 26, 25, result, &next));
    packet[26] = 25; check(!net_dns_name(packet, 27, 25, result, &next));
    packet[26] = 26; check(!net_dns_name(packet, 27, 25, result, &next));
    packet[26] = 0; check(!net_dns_name(packet, 27, 25, result, &next));
    packet[12] = 64; check(!net_dns_name(packet, 25, 12, result, &next));
    packet[12] = 63; check(!net_dns_name(packet, 25, 12, result, &next));
    packet[12] = 1; packet[13] = 'a'; packet[14] = 192; packet[15] = 12;
    check(!net_dns_name(packet, 16, 12, result, &next));
    offset = 12;
    for (i = 0; i < 4; ++i) { packet[offset++] = i == 3 ? 61 : 63; memset(packet + offset, 'a', i == 3 ? 61 : 63); offset += i == 3 ? 61 : 63; }
    packet[offset++] = 0;
    check(net_dns_name(packet, offset, 12, result, &next) && strlen(result) == 253 && next == offset);
    packet[offset - 63] = 62; packet[offset - 1] = 'a'; packet[offset++] = 0;
    check(!net_dns_name(packet, offset, 12, result, &next));
    memset(&net, 0, sizeof(net)); net.config.dns[0] = 1; net.link = 1;
    check(net_dns_start(&net, &dns, "Syma-Lang.Org.", 0xfffffff0UL, 100UL, 0) == NET_OK && !strcmp(dns.name, "syma-lang.org"));
    /* Unsigned 32-bit subtraction crosses clock wrap without a CPU-loop timer. */
    check(net_dns_poll(&dns, 0x54UL) == NET_DNS_TIMEOUT && !dns.port);
    check(net_dns_start(&net, &dns, "bad..name", 0, 100, 0) == NET_BAD_ARGUMENT &&
          net_dns_start(&net, &dns, "-bad.example", 0, 100, 0) == NET_BAD_ARGUMENT &&
          net_dns_start(&net, &dns, "bad-.example", 0, 100, 0) == NET_BAD_ARGUMENT);
    check(net_dns_start(&net, &dns, "ok.example", 0, 0, 0) == NET_BAD_ARGUMENT &&
          net_dns_start(&net, &dns, "ok.example", 0, 100, 5) == NET_BAD_ARGUMENT);
    check(net_dns_start(&net, &dns, "ok.example", 0, 100, 0) == NET_OK);
    net_dns_cancel(&dns);
    check(dns.status == NET_DNS_CANCELLED && !dns.port && net_dns_start(&net, &dns, "ok.example", 0, 100, 0) == NET_OK);
    net_dns_cancel(&dns);
    putchar('\n'); return 0;
}
