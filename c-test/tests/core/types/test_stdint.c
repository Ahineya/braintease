#include <stdint.h>
#include <stdio.h>

int main() {
    int8_t a;
    uint8_t b;
    int16_t c;
    uint16_t d;
    int32_t e;
    uint32_t f;

    a = 127;
    if (a == 127) putchar('Y'); else putchar('N');

    b = 255;
    if (b == 255) putchar('Y'); else putchar('N');

    c = 32767;
    if (c == 32767) putchar('Y'); else putchar('N');

    d = 40000U;
    if (d == 40000U) putchar('Y'); else putchar('N');

    e = 65536;
    if (e == 65536) putchar('Y'); else putchar('N');

    f = 80000UL;
    if (f == 80000UL) putchar('Y'); else putchar('N');

    if (sizeof(a) == 1) putchar('Y'); else putchar('N');
    if (sizeof(c) == 2) putchar('Y'); else putchar('N');
    if (sizeof(e) == 4) putchar('Y'); else putchar('N');

    if (INT32_MAX == 2147483647L) putchar('Y'); else putchar('N');
    if (UINT32_MAX == 4294967295UL) putchar('Y'); else putchar('N');
    if (INT32_C(1) + 65535 == 65536) putchar('Y'); else putchar('N');

    if (SIZE_MAX == 65535U) putchar('Y'); else putchar('N');

    putchar('\n');
    return 0;
}
