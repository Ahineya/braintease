#ifndef RIPPLE_DHCP_INTERNAL_H
#define RIPPLE_DHCP_INTERNAL_H
#include "internal.h"
#include <net_dhcp.h>
#define DHCP_MASK 1
#define DHCP_ROUTER 2
#define DHCP_DNS 4
#define DHCP_LEASE 8
#define DHCP_SERVER 16
#define DHCP_TYPE 32
#define DHCP_T1 64
#define DHCP_T2 128
#define DHCP_OVERLOAD 256

typedef struct DhcpReply {
    NetConfig config;
    unsigned char server[4];
    unsigned long lease, t1, t2;
    unsigned short seen, type, overload;
} DhcpReply;
unsigned long dhcp_read32(const unsigned char *p);
void dhcp_write32(unsigned char *p, unsigned long value);
int dhcp_ip(const unsigned char *p);
int dhcp_decode(const unsigned char *p, unsigned short length, DhcpReply *reply);
int dhcp_send(NetDhcp *dhcp, unsigned short type);
void dhcp_receive(NetContext *net, const NetDatagram *packet, void *user);
void dhcp_begin(NetDhcp *dhcp, unsigned long now_ms);
int dhcp_fail(NetDhcp *dhcp, int error);
int dhcp_probe(NetDhcp *dhcp, unsigned long now_ms);
int dhcp_conflict(NetDhcp *dhcp, unsigned long now_ms);
#endif
