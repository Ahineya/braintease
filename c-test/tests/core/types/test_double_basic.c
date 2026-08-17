#include <stdio.h>

void putchar(int c);

double addd(double a, double b) {
    return a + b;
}

int main() {
    if (1.0 + 2.0 == 3.0) putchar('Y'); else putchar('N');
    if (2.0 * 3.0 == 6.0) putchar('Y'); else putchar('N');
    if (6.0 / 2.0 == 3.0) putchar('Y'); else putchar('N');
    if (3.0 - 1.0 == 2.0) putchar('Y'); else putchar('N');

    if (2.0 < 3.0) putchar('Y'); else putchar('N');
    if (addd(1.5, 2.5) == 4.0) putchar('Y'); else putchar('N');

    if ((int)3.7 == 3) putchar('Y'); else putchar('N');
    if ((double)2 == 2.0) putchar('Y'); else putchar('N');

    double f = 1.0f;
    if (f == 1.0) putchar('Y'); else putchar('N');

    putchar('\n');
    return 0;
}
