#include <stdio.h>
#include <limits.h>

void putchar(int c);

long add1(long x) {
    return x + 1;
}

int main() {
    long a = 65535;
    if (a + 1 == 65536) putchar('Y'); else putchar('N');

    long b = -1;
    if (b + 1 == 0) putchar('Y'); else putchar('N');

    long c = 65536;
    if (c - 1 == 65535) putchar('Y'); else putchar('N');

    if (c > a) putchar('Y'); else putchar('N');
    if (b < 0) putchar('Y'); else putchar('N');

    long d = 1000;
    if (d * 1000 == 1000000) putchar('Y'); else putchar('N');

    long e = 1000000;
    if (e / 1000 == 1000) putchar('Y'); else putchar('N');

    unsigned long u = 40000;
    if (u + u == 80000) putchar('Y'); else putchar('N');

    if (LONG_MAX == 2147483647L) putchar('Y'); else putchar('N');
    if (LONG_MIN == (-2147483647L-1)) putchar('Y'); else putchar('N');
    if (ULONG_MAX == 4294967295UL) putchar('Y'); else putchar('N');

    long x = 65535;
    x++;
    if (x == 65536) putchar('Y'); else putchar('N');

    int n = 2;
    long m = n;
    if (m + 65535 == 65537) putchar('Y'); else putchar('N');

    if (add1(65535) == 65536) putchar('Y'); else putchar('N');

    long bits = 0x00010000L;
    if ((bits >> 16) == 1) putchar('Y'); else putchar('N');
    if ((1L << 16) == 65536) putchar('Y'); else putchar('N');

    putchar('\n');
    return 0;
}
