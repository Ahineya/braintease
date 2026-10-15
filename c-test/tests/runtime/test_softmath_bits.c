#include <stdio.h>
extern unsigned long long __rcc_f64_floor(unsigned long long);
extern unsigned long long __rcc_f64_sqrt(unsigned long long);
extern unsigned long long __rcc_f64_reduce_pi2(unsigned long long, long *);
#define CHECK(x) do { if (x) putchar('Y'); else putchar('N'); } while (0)
int main(void) {
    long quadrant;
    unsigned long long fraction;
    CHECK(__rcc_f64_floor(0x41E65A0BC0100000ULL) == 0x41E65A0BC0000000ULL);
    CHECK(__rcc_f64_floor(0xC1E65A0BC0100000ULL) == 0xC1E65A0BC0200000ULL);
    CHECK(__rcc_f64_floor(0x8000000000000000ULL) == 0x8000000000000000ULL);
    CHECK(__rcc_f64_floor(1ULL) == 0ULL);
    CHECK(__rcc_f64_floor(0x8000000000000001ULL) == 0xBFF0000000000000ULL);
    CHECK(__rcc_f64_floor(0x7FF0000000000000ULL) == 0x7FF0000000000000ULL);
    CHECK(__rcc_f64_sqrt(0x4000000000000000ULL) == 0x3FF6A09E667F3BCDULL);
    CHECK(__rcc_f64_sqrt(1ULL) == 0x1E60000000000000ULL);
    CHECK(__rcc_f64_sqrt(0x7FEFFFFFFFFFFFFFULL) == 0x5FEFFFFFFFFFFFFFULL);
    CHECK(__rcc_f64_sqrt(0x8000000000000000ULL) == 0x8000000000000000ULL);
    CHECK(__rcc_f64_sqrt(0x7FF0000000000000ULL) == 0x7FF0000000000000ULL);
    CHECK(__rcc_f64_sqrt(0xBFF0000000000000ULL) == 0x7FF8000000000000ULL);
    fraction = __rcc_f64_reduce_pi2(0x7FEFFFFFFFFFFFFFULL, &quadrant);
    CHECK(quadrant == 2);
    CHECK(fraction == 0xBF69E0ACF4A728E0ULL);
    putchar('\n');
    return 0;
}
