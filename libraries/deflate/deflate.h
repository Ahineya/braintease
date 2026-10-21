/* SPDX-License-Identifier: Zlib */
#ifndef RIPPLE_DEFLATE_H
#define RIPPLE_DEFLATE_H
#include <stdint.h>
#define TINF_OK 0
#define TINF_DATA_ERROR (-3)
#define TINF_BUF_ERROR (-5)
/* Raw DEFLATE, one octet per addressable cell. Exact input consumption;
 * output length is updated only on success. No allocation or global state.
 * Source and destination must be disjoint, with their declared extents. */
int32_t ri_deflate(void *dest, uint32_t *length, const void *source, uint32_t size);
/* Streaming raw DEFLATE. next returns one octet or a negative failure; emit
 * accepts a <=4096-cell span. Uses a 32 KiB history, bounded output and no
 * lookahead past the final compressed octet. produced changes only on success. */
int32_t ri_inflate_stream(int (*next)(void *),int (*emit)(void *,const unsigned char *,unsigned),
    void *opaque,uint32_t limit,uint32_t *produced);
#endif
