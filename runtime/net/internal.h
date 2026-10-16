#ifndef RIPPLE_NET_INTERNAL_H
#define RIPPLE_NET_INTERNAL_H
#include <net.h>
#include <string.h>
/* Optional protocol attachment: UDP/ICMP programs need not link TCP's state
 * machine merely because IPv4 recognizes its protocol number. */
struct NetTcpDriver {
    int (*input)(NetContext *, unsigned short, unsigned short);
    void (*transmitted)(NetContext *);
    void (*dropped)(NetContext *);
    void (*error)(NetContext *, const NetError *);
};
int net_config_valid(const NetConfig *config);
void net_count(unsigned long *counter);
void net_reset_paths(NetContext *net);
void net_link_lost(NetContext *net);
int net_link_poll(NetContext *net, unsigned long now_ms);
void net_arp_tick(NetContext *net);
void net_arp_sent(NetContext *net);
void net_remember_packet(NetContext *net, unsigned short length);
void net_history_tick(NetContext *net);
void net_forget_udp(NetContext *net, unsigned short port);
int net_icmp_error(NetContext *net, const unsigned char *packet, unsigned short length);
unsigned short net_read16(const unsigned char *p);
void net_write16(unsigned char *p, unsigned short value);
unsigned short net_sum(unsigned short sum, const unsigned char *p, unsigned short length);
unsigned short net_checksum(const unsigned char *p, unsigned short length);
unsigned short net_udp_checksum(const unsigned char *source, const unsigned char *destination,
                                 const unsigned char *udp, unsigned short length);
unsigned short net_random(NetContext *net);
int net_transmit(NetContext *net, unsigned short length);
void net_ethernet(NetContext *net, const unsigned char *destination, unsigned short type);
void net_ipv4_header(NetContext *net, const unsigned char *destination, unsigned short protocol, unsigned short length);
int net_route(NetContext *net, const unsigned char *destination, unsigned char *mac);
int net_arp_send(NetContext *net, unsigned short operation, const unsigned char *mac, const unsigned char *address);
int net_arp_input(NetContext *net, unsigned short length);
int net_ipv4_input(NetContext *net, unsigned short length);
int net_icmp_input(NetContext *net, unsigned short header, unsigned short length);
int net_udp_input(NetContext *net, unsigned short header, unsigned short length);
unsigned short net_l4_checksum(const unsigned char *source, const unsigned char *destination,
                              unsigned short protocol, const unsigned char *data, unsigned short length);
int net_tcp_input(NetContext *net, unsigned short header, unsigned short length);
void net_tcp_transmitted(NetContext *net);
void net_tcp_pending_dropped(NetContext *net);
void net_tcp_error(NetContext *net, const NetError *error);
#endif
