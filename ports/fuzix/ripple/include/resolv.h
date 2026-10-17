/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RIPPLE_RESOLV_H
#define RIPPLE_RESOLV_H
#include <netdb.h>
#define RIPPLE_DNS_PACKET 4096
struct ripple_dns_rr {
    char name[254];
    unsigned type, klass, size, data;
    unsigned long ttl;
};
unsigned ripple_dns_u16(const unsigned char *);
unsigned long ripple_dns_u32(const unsigned char *);
/* Bounds-checked octet parsers. offset advances past the encoded field. */
int ripple_dns_name(const unsigned char *, unsigned, unsigned *, char *);
int ripple_dns_rr(const unsigned char *, unsigned, unsigned *, struct ripple_dns_rr *);
int ripple_dns_server(char *);
int ripple_dns_query(const char *server, const char *name, unsigned type,
                     unsigned char *reply, unsigned capacity);
int res_query(const char *, int, int, unsigned char *, int);
#endif
