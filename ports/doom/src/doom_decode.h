#ifndef RIPPLE_DOOM_DECODE_H
#define RIPPLE_DOOM_DECODE_H

#include <stdint.h>

/* Serialized data contains one eight-bit octet per C array element, including
 * on Ripple where a C byte occupies a 16-bit cell. Callers validate the entire
 * record range before using these field readers. No host struct layout or
 * alignment is part of the file format. */
static inline uint16_t DoomReadLE16(const unsigned char *p)
{
    return (uint16_t)((uint16_t)(p[0] & 255U)
         | (uint16_t)((uint16_t)(p[1] & 255U) << 8));
}

static inline uint32_t DoomReadLE32(const unsigned char *p)
{
    return (uint32_t)(p[0] & 255U)
         | ((uint32_t)(p[1] & 255U) << 8)
         | ((uint32_t)(p[2] & 255U) << 16)
         | ((uint32_t)(p[3] & 255U) << 24);
}

static inline int32_t DoomReadS16(const unsigned char *p)
{
    uint16_t value = DoomReadLE16(p);
    if (value & UINT16_C(0x8000))
        return (int32_t)value - INT32_C(65536);
    return (int32_t)value;
}

static inline int32_t DoomReadFixed16(const unsigned char *p)
{
    /* Multiplication defines negative coordinates without a signed left shift. */
    return DoomReadS16(p) * INT32_C(65536);
}

static inline void DoomReadName8(const unsigned char *p, char *name)
{
    int i;
    for (i = 0; i < 8; ++i)
        name[i] = (char)(p[i] & 255U);
    name[8] = 0;
}

#endif
