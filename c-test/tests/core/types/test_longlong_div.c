#include <stdio.h>

void putchar(int c);

int main() {
    long long a = 10000000000LL;
    if (a / 10 == 1000000000LL) putchar('Y'); else putchar('N');
    if (a % 10 == 0) putchar('Y'); else putchar('N');

    unsigned long long u = 1ULL << 40;
    if (u / 16 == (1ULL << 36)) putchar('Y'); else putchar('N');
    if (u % 16 == 0) putchar('Y'); else putchar('N');

    long long n = -10000000000LL;
    if (n / 10 == -1000000000LL) putchar('Y'); else putchar('N');
    if (n / -10 == 1000000000LL) putchar('Y'); else putchar('N');

    long long bits = 1LL << 40;
    if ((bits >> 8) == (1LL << 32)) putchar('Y'); else putchar('N');
    if ((-1LL >> 1) == -1LL) putchar('Y'); else putchar('N');

    unsigned long long ubits = 1ULL << 40;
    if ((ubits >> 8) == (1ULL << 32)) putchar('Y'); else putchar('N');

    if ((3LL << 40) == 3298534883328LL) putchar('Y'); else putchar('N');

    putchar('\n');
    return 0;
}
