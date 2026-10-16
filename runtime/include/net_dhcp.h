#ifndef RIPPLE_NET_DHCP_H
#define RIPPLE_NET_DHCP_H
#include <net.h>
#define NET_DHCP_STOPPED 0
#define NET_DHCP_SELECTING 1
#define NET_DHCP_REQUESTING 2
#define NET_DHCP_PROBING 3
#define NET_DHCP_BOUND 4
#define NET_DHCP_RENEWING 5
#define NET_DHCP_REBINDING 6
#define NET_DHCP_BACKOFF 7
#define NET_DHCP_FAILED 8
#define NET_DHCP_WAIT_LINK 9
/* Caller-owned, zero before first use. Never move an active client. Times in
 * lease/t1/t2/age/retry_age are seconds; fraction is milliseconds within a
 * second. Clock readings and short delays use wrapping milliseconds. */
typedef struct NetDhcp {
    NetContext *net;
    NetConfig offered;
    unsigned char server[4];
    unsigned long xid, lease, t1, t2, age, fraction, last_ms;
    unsigned long started, timeout, action_at, delay, request_at;
    unsigned long generation, acquired, renewals, expirations, malformed, retry_age, link_epoch;
    unsigned short state, active, error, attempts, probes, announced, conflicts;
    unsigned short request_sent;
} NetDhcp;
/* net_open first. timeout_ms bounds each acquisition, 1..600000. No static
 * fallback. The first acceptable offer is selected. Static UDP bindings survive
 * configuration changes, while in-flight configuration-dependent work expires. */
int net_dhcp_start(NetContext *net, NetDhcp *dhcp, unsigned long now_ms, unsigned long timeout_ms);
/* Drives lease timers BEFORE receiving one frame, then DHCP progress. Call in
 * place of net_poll for the client's lifetime, including after BOUND. Returns
 * NET_* poll status; inspect state/error for acquisition failure. Never stop
 * pumping just because the first lease was acquired. LINK_DOWN enters WAIT_LINK
 * without a timeout; restoration starts fresh acquisition with a new budget. */
int net_dhcp_poll(NetDhcp *dhcp, unsigned long now_ms);
/* Cancels locally, unconfigures and releases port 68. No DHCPRELEASE sent. */
void net_dhcp_stop(NetDhcp *dhcp);
#endif
