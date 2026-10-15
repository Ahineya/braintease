#ifndef RIPPLE_NET_DNS_H
#define RIPPLE_NET_DNS_H
#include <net.h>
#define NET_DNS_WAITING 0
#define NET_DNS_SUCCESS 1
#define NET_DNS_NXDOMAIN 2
#define NET_DNS_NO_ADDRESS 3
#define NET_DNS_SERVER_ERROR 4
#define NET_DNS_TRUNCATED 5
#define NET_DNS_TIMEOUT 6
#define NET_DNS_NETWORK_ERROR 7
#define NET_DNS_CANCELLED 8
/* Caller-owned state; initialize to zero before first use. One query at a time.
 * No heap, DNS cache, TCP fallback, DHCP or host resolver. */
typedef struct NetDns {
    NetContext *net;
    char name[254];
    unsigned char address[4];
    unsigned short status, port, identifier, attempts, limit, sent, malformed;
    unsigned long started, timeout, ttl;
} NetDns;
/* name is an ASCII hostname (optional trailing dot). retries is the number of
 * additional attempts, 0..4; timeout_ms applies to each attempt, including ARP.
 * Seed NetConfig.seed from caller entropy for unpredictable IDs/source ports.
 * poll does not receive packets: call net_poll before net_dns_poll. */
int net_dns_start(NetContext *net, NetDns *dns, const char *name,
                  unsigned long now_ms, unsigned long timeout_ms, unsigned short retries);
int net_dns_poll(NetDns *dns, unsigned long now_ms);
void net_dns_cancel(NetDns *dns);
#endif
