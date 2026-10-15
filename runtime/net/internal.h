#ifndef RIPPLE_NET_INTERNAL_H
#define RIPPLE_NET_INTERNAL_H
#include <net.h>
#include <string.h>
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
#endif
