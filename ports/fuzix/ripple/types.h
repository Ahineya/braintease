/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RIPPLE_FUZIX_TYPES_H
#define RIPPLE_FUZIX_TYPES_H
#include <stdint.h>
#include <stddef.h>
/* Historical FUZIX names: these are character carriers, NOT exact octets. */
typedef unsigned char uint8_t;
typedef signed char int8_t;
typedef uint16_t irqflags_t;
typedef int32_t arg_t;
typedef uint32_t uarg_t;
typedef uint32_t usize_t;
typedef int32_t susize_t;
typedef int32_t ssize_t;
typedef uint32_t uaddr_t;
typedef uint32_t uptr_t;
#define MAXUSIZE 0xFFFFFFFFUL
#endif
