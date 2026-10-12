#include <stdio.h>

int main(void) {
    long negative = -7L;
    long long wider_negative = -7LL;
    unsigned int small_unsigned = 3U;
    unsigned long wider_unsigned = 3UL;
    int narrow = 30000;
    long large_divisor = 70000L;

    /* The wider signed type represents all values of the unsigned operand. */
    if (negative / small_unsigned != -2L || negative % small_unsigned != -1L ||
        !(negative < small_unsigned) || negative >= small_unsigned ||
        wider_negative / wider_unsigned != -2LL ||
        wider_negative % wider_unsigned != -1LL ||
        !(wider_negative < wider_unsigned) || wider_negative >= wider_unsigned) {
        puts("common signed type failed");
        return 1;
    }

    negative /= small_unsigned;
    wider_negative /= wider_unsigned;
    if (negative != -2L || wider_negative != -2LL) return 1;
    negative = -7L;
    wider_negative = -7LL;
    negative %= small_unsigned;
    wider_negative %= wider_unsigned;
    if (negative != -1L || wider_negative != -1LL) return 1;

    /* Compound assignments calculate in the common type before narrowing. */
    narrow /= large_divisor;
    if (narrow != 0) {
        puts("compound division narrowed early");
        return 1;
    }
    narrow = 30000;
    narrow %= large_divisor;
    if (narrow != 30000) {
        puts("compound remainder narrowed early");
        return 1;
    }

    negative = -8L;
    negative >>= 1U;
    wider_negative = -8LL;
    wider_negative >>= 1UL;
    if (negative != -4L || wider_negative != -4LL) return 1;

    puts("signed common type ok");
    return 0;
}
