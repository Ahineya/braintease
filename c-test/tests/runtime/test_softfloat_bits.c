#include <stdio.h>

/* Call the integer ABI directly so constant folding cannot mask runtime errors. */
extern unsigned long __rcc_f32_add(unsigned long, unsigned long);
extern unsigned long __rcc_f32_sub(unsigned long, unsigned long);
extern unsigned long __rcc_f32_mul(unsigned long, unsigned long);
extern unsigned long __rcc_f32_div(unsigned long, unsigned long);
extern unsigned long __rcc_f32_from_ui(unsigned long);
extern unsigned long __rcc_f32_from_i64(long long);
extern unsigned long __rcc_f32_from_u64(unsigned long long);
extern unsigned long __rcc_f32_to_ui(unsigned long);
extern long long __rcc_f32_to_i64(unsigned long);
extern unsigned long long __rcc_f32_to_u64(unsigned long);
extern unsigned long __rcc_f32_from_f64(unsigned long long);
extern unsigned long long __rcc_f64_from_f32(unsigned long);
extern unsigned long long __rcc_f64_add(unsigned long long, unsigned long long);
extern unsigned long long __rcc_f64_sub(unsigned long long, unsigned long long);
extern unsigned long long __rcc_f64_mul(unsigned long long, unsigned long long);
extern unsigned long long __rcc_f64_div(unsigned long long, unsigned long long);

#define CHECK(x) do { if (x) putchar('Y'); else putchar('N'); } while (0)

int main(void) {
    CHECK(__rcc_f32_add(1UL, 1UL) == 2UL);
    CHECK(__rcc_f32_sub(1UL, 2UL) == 0x80000001UL);
    CHECK(__rcc_f32_add(0x3F800001UL, 0x33800000UL) == 0x3F800002UL);
    CHECK(__rcc_f32_add(0x3F800000UL, 0x33800000UL) == 0x3F800000UL);
    CHECK(__rcc_f32_mul(1UL, 0x3F800000UL) == 1UL);
    CHECK(__rcc_f32_mul(0x3FA00001UL, 0x3FC00001UL) == 0x3FF00003UL);
    CHECK(__rcc_f32_mul(3UL, 0x3F000000UL) == 2UL);
    CHECK(__rcc_f32_div(0x3F800000UL, 0x40400000UL) == 0x3EAAAAABUL);
    CHECK(__rcc_f32_div(1UL, 3UL) == 0x3EAAAAABUL);
    CHECK(__rcc_f32_div(3UL, 0x40000000UL) == 2UL);
    CHECK(__rcc_f32_from_ui(0xFFFFFFFFUL) == 0x4F800000UL);
    CHECK(__rcc_f32_from_i64((long long)0x8000000000000000ULL) == 0xDF000000UL);
    CHECK(__rcc_f32_from_u64(0xFFFFFFFFFFFFFFFFULL) == 0x5F800000UL);
    CHECK(__rcc_f32_to_ui(0x4F400000UL) == 0xC0000000UL);
    CHECK(__rcc_f32_to_i64(0xD3800000UL) == -1099511627776LL);
    CHECK(__rcc_f32_to_u64(0x5F000000UL) == 0x8000000000000000ULL);
    CHECK(__rcc_f32_from_f64(0x36A8000000000000ULL) == 2UL);
    CHECK(__rcc_f32_from_f64(0x3690000000000000ULL) == 0UL);
    CHECK(__rcc_f64_from_f32(0xBF800000UL) == 0xBFF0000000000000ULL);
    CHECK(__rcc_f64_from_f32(0x80000000UL) == 0x8000000000000000ULL);
    CHECK(__rcc_f64_from_f32(0x80000001UL) == 0xB6A0000000000000ULL);
    CHECK(__rcc_f64_add(1ULL, 1ULL) == 2ULL);
    CHECK(__rcc_f64_add(0x000FFFFFFFFFFFFFULL, 1ULL) == 0x0010000000000000ULL);
    CHECK(__rcc_f64_add(0x0010000000000000ULL, 0x0010000000000000ULL) == 0x0020000000000000ULL);
    CHECK(__rcc_f64_sub(0x0010000000000000ULL, 1ULL) == 0x000FFFFFFFFFFFFFULL);
    CHECK(__rcc_f64_div(0x3FF0000000000000ULL, 0x4008000000000000ULL) == 0x3FD5555555555555ULL);
    CHECK(__rcc_f64_div(1ULL, 3ULL) == 0x3FD5555555555555ULL);
    CHECK(__rcc_f64_div(3ULL, 0x4000000000000000ULL) == 2ULL);
    CHECK(__rcc_f64_mul(3ULL, 0x3FE0000000000000ULL) == 2ULL);
    CHECK(__rcc_f64_mul(0x7FEFFFFFFFFFFFFFULL, 0x4000000000000000ULL) == 0x7FF0000000000000ULL);
    CHECK(__rcc_f64_mul(0x8000000000000000ULL, 0x3FE0000000000000ULL) == 0x8000000000000000ULL);
    CHECK(__rcc_f64_div(0xBFF8000000000000ULL, 0x3FE0000000000000ULL) == 0xC008000000000000ULL);
    CHECK(__rcc_f64_mul(0x3FF0000000000001ULL, 0x3FF0000000000001ULL) == 0x3FF0000000000002ULL);
    CHECK(__rcc_f64_mul(0x3FF4000000000001ULL, 0x3FF8000000000001ULL) == 0x3FFE000000000003ULL);
    CHECK(__rcc_f64_mul(0x0010000000000001ULL, 0x3FE0000000000001ULL) == 0x0008000000000001ULL);
    CHECK(__rcc_f64_mul(0x7FEFFFFFFFFFFFFFULL, 0x3FEFFFFFFFFFFFFFULL) == 0x7FEFFFFFFFFFFFFEULL);
    putchar('\n');
    return 0;
}
