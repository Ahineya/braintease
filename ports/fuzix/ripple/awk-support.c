/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include "../awk/awk.h"
uint32_t __float_bits(float value) {
    union { float value; uint32_t bits; } u;
    u.value = value;
    return u.bits;
}
uint64_t __double_bits(double value) {
    union { double value; uint64_t bits; } u;
    u.value = value;
    return u.bits;
}
void __force_eval(float value) { (void)value; }
int awk_getc(FILE *stream) {
    int c = fgetc(stream);
    if ((c == EOF && !feof(stream) && !ferror(stream)) ||
        (c != EOF && (unsigned)c > 255))
        FATAL("input contains a non-octet character");
    if (ferror(stream)) FATAL("input read error");
    return c;
}

int awk_octet(int value) {
    if ((unsigned)value > 255) FATAL("regex input contains a non-octet character");
    return value;
}
