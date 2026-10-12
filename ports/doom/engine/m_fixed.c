//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	Fixed point implementation.
//



#include "doomtype.h"
#include "m_fixed.h"

/* Convert modulo-2^32 bits without an out-of-range signed cast. */
static int32_t SignedBits(uint32_t bits)
{
    if (bits <= (uint32_t) INT32_MAX)
        return (int32_t) bits;
    return -INT32_C(1) - (int32_t) (UINT32_MAX - bits);
}

static uint32_t Magnitude(fixed_t value)
{
    uint32_t bits = (uint32_t) value;
    return value < 0 ? UINT32_C(0) - bits : bits;
}

/* Ripple supplies both exact operations in fixed_fast.asm. */
#ifndef RIPPLE
fixed_t FixedMul(fixed_t a, fixed_t b)
{
    /* Signed multiplication fits in 64 bits. Unsigned shifting then narrowing
     * preserves vanilla's low 32 result bits, including negative products. */
    int64_t product = (int64_t) a * (int64_t) b;
    uint32_t bits = (uint32_t) ((uint64_t) product >> FRACBITS);
    return SignedBits(bits);
}

fixed_t FixedDiv(fixed_t a, fixed_t b)
{
    /* Keep Doom's conservative overflow guard, including divide-by-zero
     * saturation. Unsigned magnitude also defines the INT32_MIN case. */
    if ((Magnitude(a) >> 14) >= Magnitude(b))
        return (a < 0) != (b < 0) ? INT32_MIN : INT32_MAX;

    /* Multiplication avoids undefined left shifts of negative operands. */
    return (fixed_t) (((int64_t) a * INT64_C(65536)) / (int64_t) b);
}

#endif
