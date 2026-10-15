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
#define NET_FRAME_SIZE 1514
#define NET_ARP_ENTRIES 4
#define NET_UDP_PORTS 4
#define NET_EVENT_NONE 0
#define NET_EVENT_ECHO_REQUEST 1
#define NET_EVENT_ECHO_REPLY 2

typedef struct NetConfig {
    unsigned short interface;
    unsigned char address[4], mask[4], gateway[4], dns[4];
    unsigned short seed;
} NetConfig;
typedef struct NetContext NetContext;
typedef struct NetDatagram {
    unsigned char source[4];
    unsigned short source_port, destination_port, length;
    const unsigned char *data;
} NetDatagram;
/* Payload is valid only during the callback. Do not recursively poll context. */
typedef void (*NetUdpHandler)(NetContext *, const NetDatagram *, void *);
typedef struct NetUdpPort {
    unsigned short port;
    NetUdpHandler handler;
    void *user;
} NetUdpPort;
typedef struct NetArpEntry {
    unsigned char address[4], mac[6];
    unsigned short used, ready, attempts;
    unsigned long requested;
} NetArpEntry;
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
};

/* Initializes the NIC and announces the configured static address by ARP. */
int net_init(NetContext *net, const NetConfig *config);
/* now_ms is a caller-supplied monotonic 32-bit clock, in milliseconds. Only
 * the caller advances time. Each poll handles at most one received frame. */
int net_poll(NetContext *net, unsigned long now_ms);
int net_udp_bind(NetContext *net, unsigned short port, NetUdpHandler handler, void *user);
int net_udp_unbind(NetContext *net, unsigned short port);
int net_udp_send(NetContext *net, const unsigned char *destination, unsigned short source_port,
                 unsigned short destination_port, const unsigned char *data, unsigned short length);
int net_icmp_echo(NetContext *net, const unsigned char *destination, unsigned short identifier,
                  unsigned short sequence, const unsigned char *data, unsigned short length);
/* Send OK means the context/host queue accepted it. AGAIN requires retrying
 * with the original payload after polling; neither means remote delivery. */
#endif
