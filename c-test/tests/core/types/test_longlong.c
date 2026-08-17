#include <stdio.h>
#include <limits.h>

void putchar(int c);

long long add1(long long x) {
    return x + 1;
}

int main() {
    if (sizeof(long long) == 8) putchar('Y'); else putchar('N');
    if (sizeof(unsigned long long) == 8) putchar('Y'); else putchar('N');

    long long a = 4294967295LL;
    if (a + 1 == 4294967296LL) putchar('Y'); else putchar('N');

    long long b = -1;
    if (b + 1 == 0) putchar('Y'); else putchar('N');

    if (1LL << 32 == 4294967296LL) putchar('Y'); else putchar('N');
    if ((1LL << 33) >> 33 == 1) putchar('Y'); else putchar('N');

    long long c = 10000000000LL;
    if (c / 1000 == 10000000LL) putchar('Y'); else putchar('N');
    if (c * 2 == 20000000000LL) putchar('Y'); else putchar('N');

    unsigned long long u = 1ULL << 40;
    if (u + u == (1ULL << 41)) putchar('Y'); else putchar('N');

    if (LLONG_MAX == 9223372036854775807LL) putchar('Y'); else putchar('N');
    if (LLONG_MIN == (-9223372036854775807LL-1)) putchar('Y'); else putchar('N');
    if (ULLONG_MAX == 18446744073709551615ULL) putchar('Y'); else putchar('N');

    if (add1(4294967295LL) == 4294967296LL) putchar('Y'); else putchar('N');

    long n = 2;
    long long m = n;
    if (m + 4294967295LL == 4294967297LL) putchar('Y'); else putchar('N');

    if (a > 0) putchar('Y'); else putchar('N');
    if (b < 0) putchar('Y'); else putchar('N');

    putchar('\n');
    return 0;
}
