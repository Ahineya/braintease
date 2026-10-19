/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NETSURF_C16_HTTP_H
#define NETSURF_C16_HTTP_H
#include <stdbool.h>
/* Register after netsurf_init(), before navigation. The optional CA pathname
 * is passed to the independent persistent htget worker; no TLS code is linked into NetSurf. */
struct netsurf_transfer {
    unsigned long received, total;
    bool active, known;
    const char *url;
};
/* Snapshot only; strings are borrowed until the next fetch poll. */
void netsurf_http_transfer(struct netsurf_transfer *);
nserror netsurf_http_register(const char *ca_file);
#endif
