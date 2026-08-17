#include <stdio.h>

inline int add(int a, int b) {
    return a + b;
}

static inline int mul(int a, int b) {
    return a * b;
}

void copy_one(int *restrict dst, int *restrict src) {
    *dst = *src;
}

int main() {
    int x;
    int y;

    if (add(2, 3) == 5) putchar('Y'); else putchar('N');
    if (mul(4, 5) == 20) putchar('Y'); else putchar('N');

    x = 7;
    copy_one(&y, &x);
    if (y == 7) putchar('Y'); else putchar('N');

    putchar('\n');
    return 0;
}
