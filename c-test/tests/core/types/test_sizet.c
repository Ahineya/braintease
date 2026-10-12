#include <stddef.h>
#include <stdio.h>

int main() {
    size_t n;
    size_t z;

    n = sizeof(int);
    if (n == 1) putchar('Y'); else putchar('N');

    n = sizeof(long);
    if (n == 2) putchar('Y'); else putchar('N');

    /* Unsigned wrap: (size_t)0 - 1 is SIZE_MAX, not -1. */
    z = 0;
    if (z - 1 > 0) putchar('Y'); else putchar('N');
    if (z - 1 == 4294967295UL) putchar('Y'); else putchar('N');

    if (sizeof(n) == 2) putchar('Y'); else putchar('N');

    putchar('\n');
    return 0;
}
