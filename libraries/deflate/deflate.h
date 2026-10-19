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
#endif
