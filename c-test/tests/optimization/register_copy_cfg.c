#include <stdio.h>

static volatile unsigned observed;

static unsigned choose(unsigned a, unsigned b, unsigned flag) {
    unsigned value = a;
    if (flag) value = b;
    return value + a;
}

static unsigned rectangle(unsigned x, unsigned y, unsigned w, unsigned h) {
    unsigned sum = 0;
    for (unsigned dy = 0; dy < h && y + dy < 5; ++dy) {
        for (unsigned dx = 0; dx < w && x + dx < 7; ++dx) {
            unsigned column = x + dx;
            unsigned row = y + dy;
            if (column == 5 && row == 3) return sum;
            sum += row * 7 + column + 1;
        }
    }
    return sum;
}

static unsigned call_barrier(unsigned a) {
    observed = a;
    return a + 1;
}

static unsigned copies_and_calls(unsigned count) {
    unsigned sum = 0;
    unsigned value = 7;
    for (unsigned i = 0; i < count; ++i) {
        unsigned saved = value;
        value = call_barrier(value);
        if (i & 1) sum += saved;
        else sum += value;
    }
    return sum;
}

int main(void) {
    if (choose(17, 42, 0) == 34) putchar('Y'); else putchar('N');
    if (choose(17, 42, 1) == 59) putchar('Y'); else putchar('N');
    if (rectangle(0, 0, 0, 5) == 0) putchar('Y'); else putchar('N');
    if (rectangle(2, 1, 3, 2) == 87) putchar('Y'); else putchar('N');
    if (rectangle(6, 4, 4, 4) == 35) putchar('Y'); else putchar('N');
    if (rectangle(4, 3, 3, 1) == 26) putchar('Y'); else putchar('N');
    if (rectangle(65535, 0, 2, 2) == 0) putchar('Y'); else putchar('N');
    if (copies_and_calls(0) == 0) putchar('Y'); else putchar('N');
    if (copies_and_calls(4) == 36 && observed == 10) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
