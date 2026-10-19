/* SPDX-License-Identifier: MIT */
#ifndef QJS_C16_H
#define QJS_C16_H
#include <limits.h>
#include <stdint.h>
#if CHAR_BIT != 16 || INT_MAX != 32767 || LONG_MAX != 2147483647L
#error This experimental QuickJS profile requires the C16 ABI
#endif
/* One logical octet per addressable cell. This is a carrier, not uint8_t.
 * Casts do not truncate to eight bits. Writers must mask explicitly.
 */
typedef unsigned char qjs_octet_t;
typedef signed char qjs_signed_octet_carrier_t;
typedef int qjs_c_int; /* Exact SDK printf/pointer-output ABI, never widened. */
#endif
