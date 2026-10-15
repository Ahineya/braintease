/* Interactive variant of the same freestanding Ethernet/ARP/IPv4/ICMP demo.
 * Stay alive and report incoming echo requests until the host stops us.
 * Configuration (NET_LOCAL_IP, NET_MASK, NET_GATEWAY_IP) is static, not DHCP. */
#define PING_MONITOR 1
#define PING_REQUESTS 0
#define PING_RESPONSES 0
#include "ping.c"
