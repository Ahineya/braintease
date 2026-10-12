#include <stdio.h>

static volatile unsigned short x = 3;
static volatile unsigned short y = 4;
static volatile unsigned short rhs_calls = 0;

static unsigned short and_bounds(unsigned short a, unsigned short b) {
    if (a < 8 && b < 9) {
        if (a >= 8) return 91;
        if (b >= 9) return 92;
        return 1;
    }
    return 0;
}

static unsigned short or_bounds(unsigned short a, unsigned short b) {
    if (a >= 8 || b >= 9) return 0;
    if (a >= 8) return 91;
    if (b >= 9) return 92;
    return 1;
}

static unsigned short ambiguous(unsigned short a, unsigned short b) {
    if (!(a < 8 && b < 9)) return a >= 8 ? 11 : 12;
    return 13;
}

static unsigned short delayed_condition(void) {
    unsigned short condition = 0, hits = 0;
    for (unsigned short i = 0; i < 4; ++i) {
        if (condition) {
            if (i < 2) return 99;
            ++hits;
        }
        condition = i != 0;
    }
    return hits;
}

int main(void) {
    if (and_bounds(x, y) == 1 && or_bounds(x, y) == 1) putchar('Y'); else putchar('N');
    x = 8;
    if (and_bounds(x, y) == 0 && or_bounds(x, y) == 0 && ambiguous(x, y) == 11) putchar('Y'); else putchar('N');
    x = 3; y = 9;
    if (and_bounds(x, y) == 0 && or_bounds(x, y) == 0 && ambiguous(x, y) == 12) putchar('Y'); else putchar('N');
    x = 65535; y = 0;
    if (and_bounds(x, y) == 0 && or_bounds(x, y) == 0) putchar('Y'); else putchar('N');
    x = 3; y = 4;
    if (ambiguous(x, y) == 13) putchar('Y'); else putchar('N');

    rhs_calls = 0; x = 8;
    if (x < 8 && (++rhs_calls, y < 9)) rhs_calls = 99;
    if (rhs_calls == 0) putchar('Y'); else putchar('N');
    x = 3;
    if (x < 8 && (++rhs_calls, y < 9)) {
        if (x >= 8) rhs_calls = 99;
    }
    if (rhs_calls == 1) putchar('Y'); else putchar('N');

    rhs_calls = 0; x = 8;
    if (!(x >= 8 || (++rhs_calls, y >= 9))) rhs_calls = 99;
    if (rhs_calls == 0) putchar('Y'); else putchar('N');
    x = 3;
    if (x >= 8 || (++rhs_calls, y >= 9)) rhs_calls = 99;
    if (rhs_calls == 1) putchar('Y'); else putchar('N');
    if (delayed_condition() == 2) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
