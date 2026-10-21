#include <stdarg.h>
static unsigned long forwarded(va_list list)
{
    unsigned i;
    unsigned long n = va_arg(list, unsigned long);
    for (i = 0; i < 9; i++) n += i;
    return n;
}
static int check(int tag, ...)
{
    va_list a, b;
    unsigned long n;
    unsigned long long wide;
    double number;
    va_start(a, tag); va_copy(b, a);
    n = forwarded(a);
    if (n != 0x12345678UL + 36UL || va_arg(b, unsigned long) != 0x12345678UL) return 1;
    n = va_arg(a, unsigned long);
    wide = va_arg(a, unsigned long long);
    number = va_arg(a, double);
    va_end(a); va_end(b);
    return tag != 7 || n != 0x12345678UL || wide != 0xFEDCBA9876543210ULL || number != 1.5;
}
int main(void)
{
    unsigned char pad[63998];
    float f = 1.5f;
    int result;
    pad[0] = 511;
    result = check(7, 0x12345678UL, 0xFEDCBA9876543210ULL, f);
    return result || pad[0] != 511;
}
