#include <stdio.h>

enum {
    A = 1 << 2,
    B,
    C = 1 + 2 * 3
};

int main() {
    if (A == 4) putchar('Y'); else putchar('N');
    if (B == 5) putchar('Y'); else putchar('N');
    if (C == 7) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
