#ifndef NETSURF_C16_H
#define NETSURF_C16_H
#include <stdint.h>

/* NetSurf's buildsystem supplies this compiler-specific alignment marker.
 * RCC's cell ABI requires no extra alignment, like the upstream Norcroft
 * profile. Leaving it undefined would instead declare a global variable. */
#define _ALIGNED

/* A protocol octet occupies one addressable cell, as in guest file/socket I/O.
 * This is deliberately not a definition of the nonexistent C16 uint8_t. */
typedef unsigned char ns_octet_t;

/* LibParserUtils' conversion pivot is four serialized octets per scalar.
 * Casting it to uint32_t* assumes CHAR_BIT == 8 and is invalid on C16. */
static inline uint32_t ns_read_be32(const ns_octet_t *s)
{
    return ((uint32_t)s[0] << 24) | ((uint32_t)s[1] << 16) |
           ((uint32_t)s[2] << 8) | (uint32_t)s[3];
}

static inline void ns_write_be32(ns_octet_t *s, uint32_t value)
{
    s[0] = (value >> 24) & 255;
    s[1] = (value >> 16) & 255;
    s[2] = (value >> 8) & 255;
    s[3] = value & 255;
}
#endif
