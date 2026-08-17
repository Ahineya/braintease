#include <stdio.h>

void putchar(int c);

double addd(double a, double b) {
    return a + b;
}

double muld(double a, double b) {
    return a * b;
}

int main() {
    /* 2^24+1 is exact in binary64, not in binary32. */
    double two24 = 16777216.0;
    if (two24 + 1.0 != two24) putchar('Y'); else putchar('N');
    if (two24 + 1.0 == 16777217.0) putchar('Y'); else putchar('N');

    /* 2^40+1 is exact in binary64. */
    long long p40 = 1;
    p40 = p40 << 40;
    double d40 = (double)p40;
    if (d40 + 1.0 == (double)(p40 + 1)) putchar('Y'); else putchar('N');
    if ((long long)(d40 + 1.0) == p40 + 1) putchar('Y'); else putchar('N');

    if (0.5 * 0.5 == 0.25) putchar('Y'); else putchar('N');
    if (10.0 / 4.0 == 2.5) putchar('Y'); else putchar('N');
    if ((-2.0) * 3.0 == -6.0) putchar('Y'); else putchar('N');
    if (9.0 / 3.0 == 3.0) putchar('Y'); else putchar('N');

    if (addd(two24, 1.0) == 16777217.0) putchar('Y'); else putchar('N');
    if (muld(d40, 2.0) == (double)(p40 << 1)) putchar('Y'); else putchar('N');

    /* Truncate toward zero. */
    if ((int)(-3.7) == -3) putchar('Y'); else putchar('N');

    /* Signed zeros compare equal. */
    double pz = 0.0;
    double nz = -0.0;
    if (pz == nz) putchar('Y'); else putchar('N');

    /* Division by zero -> Inf; 0/0 -> NaN. */
    double inf = 1.0 / pz;
    if (inf > 0.0 && inf + inf == inf) putchar('Y'); else putchar('N');
    double nan = pz / pz;
    if (nan != nan) putchar('Y'); else putchar('N');

    putchar('\n');
    return 0;
}
