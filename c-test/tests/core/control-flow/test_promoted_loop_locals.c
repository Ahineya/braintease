#include <stdio.h>

static int rotate(int count) {
    int x = 3, y = 7;
    while (count--) {
        int old = x;
        x = y;
        y = old;
    }
    return x * 10 + y;
}

static unsigned long wide_loop(unsigned int count) {
    unsigned long x = 0x12345678UL, y = 0x89abcdefUL;
    unsigned int i;
    for (i = 0; i < count; i++) {
        unsigned long old = x;
        if (i & 1) { x += 0x10003UL; }
        else { x = y; y = old; }
    }
    return x ^ y;
}

static int nested(int limit) {
    int i, j, sum = 0;
    for (i = 0; i < limit; i++) {
        for (j = 0; j < 7; j++) {
            if (j == 2) continue;
            if (i == 3 && j == 5) break;
            if ((i + j) & 1) sum += i + j;
            else sum -= j;
        }
    }
    return sum;
}

static int escaped(int n) {
    int x = 1, sum = 0;
    volatile int *p = &x;
    while (n--) { *p = x + 3; sum += x; }
    return sum;
}

int main(void) {
    if (rotate(0) == 37 && rotate(1) == 73 && rotate(18) == 37) putchar('Y'); else putchar('N');
    if (wide_loop(0) == 0x9b9f9b97UL && wide_loop(3) == 0x9b989b8aUL) putchar('Y'); else putchar('N');
    if (nested(0) == 0 && nested(4) == 14) putchar('Y'); else putchar('N');
    if (escaped(5) == 50) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
