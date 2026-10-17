/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Bit representations used by the shared FUZIX math sources. */
#include <stdint.h>
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
