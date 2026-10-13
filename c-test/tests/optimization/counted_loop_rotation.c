#include <stdio.h>

static volatile unsigned int visits;
static volatile unsigned int last;
static volatile unsigned int limit;

static void signed_loop(int start, int end) {
    for (int i = start; i < end; ++i) {
        visits = visits + 1;
        last = (unsigned int)i;
    }
}

static void unsigned_loop(unsigned int start, unsigned int end) {
    for (unsigned int i = start; i < end; ++i) {
        visits = visits + 1;
        last = i;
    }
}

static void reset(void) {
    visits = 0;
    last = 123;
}

int main(void) {
    reset();
    signed_loop(7, 3);
    if (visits == 0 && last == 123) putchar('Y'); else putchar('N');

    reset();
    unsigned_loop(0, 0);
    if (visits == 0 && last == 123) putchar('Y'); else putchar('N');

    reset();
    signed_loop(-32768, -32765);
    if (visits == 3 && last == (unsigned int)-32766) putchar('Y'); else putchar('N');

    reset();
    signed_loop(32765, 32767);
    if (visits == 2 && last == 32766) putchar('Y'); else putchar('N');

    reset();
    unsigned_loop(65533, 65535);
    if (visits == 2 && last == 65534) putchar('Y'); else putchar('N');

    reset();
    unsigned_loop(65535, 0);
    if (visits == 0 && last == 123) putchar('Y'); else putchar('N');

    reset();
    signed_loop(-2, 2);
    if (visits == 4 && last == 1) putchar('Y'); else putchar('N');

    /* A changing volatile bound must be read on every condition evaluation. */
    reset();
    limit = 7;
    for (unsigned int i = 0; i < limit; ++i) {
        visits = visits + 1;
        limit = limit - 1;
    }
    if (visits == 4 && limit == 3) putchar('Y'); else putchar('N');

    /* Live-out counters, non-unit steps and early exits retain their values. */
    reset();
    unsigned int i;
    for (i = 0; i < 7; i += 2) visits = visits + 1;
    if (visits == 4 && i == 8) putchar('Y'); else putchar('N');

    reset();
    for (i = 0; i < 7; ++i) {
        if (i == 3) break;
        visits = visits + 1;
    }
    if (visits == 3 && i == 3) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
