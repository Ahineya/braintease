#include <stdlib.h>

void putchar(int c);

int abs(int j) {
    if (j < 0) {
        return -j;
    }
    return j;
}

long labs(long j) {
    if (j < 0) {
        return -j;
    }
    return j;
}

void exit(int status) {
    (void)status;
    __asm__("HALT");
}

void abort(void) {
    putchar('a');
    putchar('b');
    putchar('o');
    putchar('r');
    putchar('t');
    putchar('\n');
    __asm__("HALT");
}
