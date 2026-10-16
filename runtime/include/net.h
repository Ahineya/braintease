#ifndef RIPPLE_NET_H
#define RIPPLE_NET_H
#include <vm_network.h>

/* Guest protocols, one wire octet per C byte/cell. No heap or host sockets.
 * Polling and callbacks are synchronous; serialize use of each context. */
#define NET_OK 0
#define NET_AGAIN 1
#define NET_BAD_ARGUMENT 2
#define NET_IO_ERROR 3
#define NET_UNREACHABLE 4
#define NET_NO_PORT 5
#define NET_REJECTED 6
#define NET_CONFIG_CHANGED 7
#define NET_TIMEOUT 8
#define NET_LINK_DOWN 9
#define NET_FRAME_SIZE 1514
#define NET_ARP_ENTRIES 4
#define NET_UDP_PORTS 4
#define NET_TX_HISTORY 8
#define NET_ARP_TTL_MS 60000UL
#define NET_ARP_COOLDOWN_MS 5000UL
#define NET_ERROR_TTL_MS 30000UL
#define NET_EVENT_NONE 0
#define NET_EVENT_ECHO_REQUEST 1
#define NET_EVENT_ECHO_REPLY 2
#define NET_EVENT_LINK_DOWN 3
#define NET_EVENT_LINK_UP 4
#define NET_EVENT_ICMP_ERROR 5

typedef struct NetConfig {
    unsigned short interface;
    unsigned char address[4], mask[4], gateway[4], dns[4];
    unsigned short seed;
} NetConfig;
typedef struct NetContext NetContext;
typedef struct NetError {
    unsigned char reporter[4], destination[4];
    unsigned short type, code, protocol, packet_id, mtu;
    unsigned short source_port, destination_port, identifier, sequence;
} NetError;
typedef struct NetDatagram {
    unsigned char source[4];
    unsigned short source_port, destination_port, length;
    const unsigned char *data;
} NetDatagram;
/* Payload is valid only during the callback. Do not recursively poll context. */
typedef void (*NetUdpHandler)(NetContext *, const NetDatagram *, void *);
typedef void (*NetUdpErrorHandler)(NetContext *, const NetError *, void *);
typedef struct NetUdpPort {
    unsigned short port;
    NetUdpHandler handler;
    NetUdpErrorHandler error_handler;
    void *user;
} NetUdpPort;
typedef struct NetArpEntry {
    unsigned char address[4], mac[6];
    unsigned short used, ready, attempts, failed;
    unsigned long requested, resolved;
} NetArpEntry;
typedef struct NetSentPacket {
    unsigned char destination[4], prefix[8];
    unsigned short used, protocol, packet_id, length;
    unsigned long sent;
} NetSentPacket;
typedef struct NetCounters {
    /* Saturating counters; host queue/device counters remain in NET_STATS. */
    unsigned long arp_queries, arp_expired, arp_failed, arp_updates;
    unsigned long tx_blocked, tx_discarded, link_down, link_up;
    unsigned long icmp_errors, icmp_unmatched;
} NetCounters;
struct NetContext {
    NetConfig config;
    unsigned char mac[6];
    unsigned char rx[NET_FRAME_SIZE], tx[NET_FRAME_SIZE];
    NetArpEntry arp[NET_ARP_ENTRIES];
    NetUdpPort ports[NET_UDP_PORTS];
    unsigned short next_arp, pending, packet_id, random;
    unsigned long now;
    unsigned long rejected, icmp_requests;
    unsigned short event, identifier, sequence, payload_length;
    unsigned char peer[4];
    const unsigned char *payload;
    unsigned long generation;
    unsigned short dhcp, conflict;
    unsigned char watch[4];
    unsigned short link, next_sent;
    unsigned long link_checked, link_epoch;
    NetSentPacket sent[NET_TX_HISTORY];
    NetError error;
    NetCounters counters;
};

/* Initializes the NIC and installs static configuration, announcing when up.
 * Initialization can succeed while disconnected; sends return LINK_DOWN. */
int net_init(NetContext *net, const NetConfig *config);
/* Open an unconfigured NIC for DHCP; it cannot send application traffic yet. */
int net_open(NetContext *net, unsigned short interface, unsigned short seed);
/* Applying a changed address/mask/gateway/DNS clears ARP and pending TX and
 * increments generation, invalidating operations using the old configuration.
 * Already accepted host frames cannot be recalled. UDP bindings survive. */
int net_configure(NetContext *net, const NetConfig *config);
void net_unconfigure(NetContext *net);
/* now_ms is a caller-supplied monotonic 32-bit clock, in milliseconds. Only
 * the caller advances time. Each poll checks link/timers and handles at most
 * one received frame. Observe link_epoch across calls, including restoration. */
int net_poll(NetContext *net, unsigned long now_ms);
int net_udp_bind(NetContext *net, unsigned short port, NetUdpHandler handler, void *user);
int net_udp_unbind(NetContext *net, unsigned short port);
/* Optional synchronous matched-ICMP notification, using the binding's user.
 * Unbind/cancel also forgets that port's pending frame and ICMP history. */
int net_udp_on_error(NetContext *net, unsigned short port, NetUdpErrorHandler handler);
/* Invalidate one next-hop neighbor, or all neighbors if address is null. */
void net_arp_invalidate(NetContext *net, const unsigned char *address);
int net_udp_send(NetContext *net, const unsigned char *destination, unsigned short source_port,
                 unsigned short destination_port, const unsigned char *data, unsigned short length);
int net_icmp_echo(NetContext *net, const unsigned char *destination, unsigned short identifier,
                  unsigned short sequence, const unsigned char *data, unsigned short length);
/* Send OK means the context/host queue accepted it. AGAIN requires retrying
 * with the original payload after polling; neither means remote delivery. */
#endif
