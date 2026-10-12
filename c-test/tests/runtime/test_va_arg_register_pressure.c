#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

static uint32_t cell = 0x89abcdefUL;

/* Keep scalar and float parameters plus eight independent wide values live
 * while va_arg loads low/high or address/bank pairs. Float variadic values
 * promote to double in C, so F32 pressure comes from the named parameter and
 * the pointer to a local float instead of extracting an invalid float vararg. */
static int probe(uint32_t seed, float scale, int marker, ...)
{
    va_list ap;
    uint32_t a0 = seed + 0x11112222UL;
    uint32_t a1 = seed + 0x22224444UL;
    uint32_t a2 = seed + 0x33336666UL;
    uint32_t a3 = seed + 0x44448888UL;
    uint32_t a4 = seed + 0x5555aaaaUL;
    uint32_t a5 = seed + 0x6666ccccUL;
    uint32_t a6 = seed + 0x7777eeeeUL;
    uint32_t a7 = seed + 0x88881111UL;
    uint32_t word;
    int32_t negative;
    uint32_t *pointer;
    float *float_pointer;
    va_start(ap, marker);
    word = va_arg(ap, uint32_t);
    negative = va_arg(ap, int32_t);
    pointer = va_arg(ap, uint32_t *);
    float_pointer = va_arg(ap, float *);
    va_end(ap);
    if (word != 0xfedcba98UL) return 1;
    if (negative != -123456789L) return 2;
    if (pointer != &cell) return 3;
    if (*pointer != 0x89abcdefUL) return 4;
    if (*float_pointer != 2.5f) return 5;
    if (scale != 1.5f || marker != 7) return 6;
    if ((a0 ^ a1 ^ a2 ^ a3 ^ a4 ^ a5 ^ a6 ^ a7) != 0x080d8171UL) return 7;
    return 0;
}
int main(void)
{
    float local = 2.5f;
    int result = probe(0x12345678UL, 1.5f, 7, 0xfedcba98UL, -123456789L, &cell, &local);
    if (result) {
        putchar('0' + result);
        putchar('\n');
        return result;
    }
    puts("vararg register pressure ok");
    return 0;
}
