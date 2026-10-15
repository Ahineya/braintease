#ifndef NET_DNS_INTERNAL_H
#define NET_DNS_INTERNAL_H
#include "internal.h"
#include <net_dns.h>
/* Decodes a bounded, backward-compressed wire name; next is the consumed
 * offset in the original stream, independent of pointer traversal. */
int net_dns_name(const unsigned char *message, unsigned short length,
                 unsigned short offset, char *name, unsigned short *next);
void net_dns_receive(NetContext *net, const NetDatagram *packet, void *user);
void net_dns_finish(NetDns *dns, unsigned short status);
#endif
