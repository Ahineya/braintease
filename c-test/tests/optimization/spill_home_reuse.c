#include <stdio.h>
static unsigned short identity(unsigned short x) {
    unsigned short out;
    __asm__("ADD %0, %1, R0" : "=r"(out) : "r"(x));
    return out;
}
static unsigned short sum(unsigned short base, unsigned short count) {
    unsigned short total = 0;
    for (unsigned short i = 0; i < count; ++i) {
        unsigned short x = identity(base);
        if (i & 1) total += x + i;
        else total += x - i;
    }
    return total;
}
static unsigned long wide(unsigned long base, unsigned short count) {
    unsigned long out = base;
    for (unsigned short i = 0; i < count; ++i) {
        if (i & 1) out += base;
        else out += 65537L;
    }
    return out;
}
int main(void) {
    if (sum(100, 10) == 1005) putchar('Y'); else putchar('N');
    if (sum(50000, 4) == 3394) putchar('Y'); else putchar('N');
    if (sum(7, 0) == 0) putchar('Y'); else putchar('N');
    if (wide(70000L, 4) == 341074L) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
