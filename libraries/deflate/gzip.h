/* SPDX-License-Identifier: Zlib */
#ifndef RIPPLE_GZIP_H
#define RIPPLE_GZIP_H
#include "deflate.h"
/* Bounded RFC 1952 stream, including header CRC, trailer CRC/size and at most
 * sixteen concatenated members. next: octet, -1 EOF, <-1 I/O error. emit accepts
 * <=4096 octets. No full-source/output allocation; produced changes on success.
 * Output may already have been emitted when a trailer or later member fails. */
int32_t ri_gzip_stream(int (*next)(void *),int (*emit)(void *,const unsigned char *,unsigned),
    void *opaque,uint32_t input_limit,uint32_t output_limit,uint32_t *produced);
#endif
