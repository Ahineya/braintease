#include <stdio.h>

#define HEX_VALUE 0x1.8p+1
#define PASTE(a, b) a##b
static const double table[] = {0X.8P+1, -0x0p0, 0x1p1008};

static unsigned long long bits64(double value)
{
    union { double value; unsigned long long bits; } u;
    u.value = value;
    return u.bits;
}

static unsigned long bits32(float value)
{
    union { float value; unsigned long bits; } u;
    u.value = value;
    return u.bits;
}

int main(void)
{
    if (HEX_VALUE == 3.0 && PASTE(0x1p, 2) == 4.0) putchar('Y'); else putchar('N');
    if (table[0] == 1.0) putchar('Y'); else putchar('N');
    if (bits64(table[1]) == 0x8000000000000000ULL) putchar('Y'); else putchar('N');
    if (bits64(table[2]) == 0x7ef0000000000000ULL) putchar('Y'); else putchar('N');
    if (bits64(0x1p-1074) == 1ULL) putchar('Y'); else putchar('N');
    if (bits64(0x1p-1075) == 0ULL) putchar('Y'); else putchar('N');
    if (bits64(0x1.000000000000000000001p-1075) == 1ULL) putchar('Y'); else putchar('N');
    if (bits64(0x1.fffffffffffffp1023) == 0x7fefffffffffffffULL) putchar('Y'); else putchar('N');
    if (bits32(0x1p-149f) == 1UL) putchar('Y'); else putchar('N');
    if (bits32(0x1.000001p0f) == 0x3f800000UL) putchar('Y'); else putchar('N');
    if (bits32(0x1.000001000000000000001p0f) == 0x3f800001UL) putchar('Y'); else putchar('N');
    if (bits32(0x1.000003p0f) == 0x3f800002UL) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
