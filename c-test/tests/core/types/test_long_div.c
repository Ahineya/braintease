#include <stdio.h>

void putchar(int c);

int main() {
    if (1000000L / 1000L == 1000L) putchar('Y'); else putchar('N');
    if ((-1000000L) / 1000L == -1000L) putchar('Y'); else putchar('N');
    if (1000000L / (-1000L) == -1000L) putchar('Y'); else putchar('N');
    if ((-1000000L) / (-1000L) == 1000L) putchar('Y'); else putchar('N');

    if (1000000L % 1000L == 0) putchar('Y'); else putchar('N');
    if ((-7L) % 3L == -1L) putchar('Y'); else putchar('N');
    if (7L % (-3L) == 1L) putchar('Y'); else putchar('N');

    unsigned long u = 4000000000UL;
    if (u / 1000UL == 4000000UL) putchar('Y'); else putchar('N');
    if (u % 1000UL == 0) putchar('Y'); else putchar('N');

    if ((1UL << 31) == 2147483648UL) putchar('Y'); else putchar('N');
    if ((0x80000000UL >> 31) == 1UL) putchar('Y'); else putchar('N');
    if ((-2L >> 1) == -1L) putchar('Y'); else putchar('N');

    putchar('\n');
    return 0;
}
