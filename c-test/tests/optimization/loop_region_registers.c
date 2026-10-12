#include <stdio.h>

static unsigned short clipped_sum(unsigned short x, unsigned short y,
                                  unsigned short width, unsigned short height,
                                  unsigned short limit_x, unsigned short limit_y,
                                  unsigned short seed) {
    unsigned short sum = seed;
    for (unsigned short row = 0; row < height && y + row < limit_y; ++row) {
        unsigned short base = (y + row) * limit_x;
        for (unsigned short column = 0; column < width && x + column < limit_x; ++column) {
            unsigned short position = base + x + column;
            if ((position & 3) == 1) sum += position;
            else sum += 3;
        }
    }
    return sum;
}

static unsigned short reference(unsigned short x, unsigned short y,
                                unsigned short width, unsigned short height,
                                unsigned short limit_x, unsigned short limit_y,
                                unsigned short seed) {
    volatile unsigned short row = 0;
    volatile unsigned short column;
    unsigned short sum = seed;
    while (row < height && y + row < limit_y) {
        column = 0;
        while (column < width && x + column < limit_x) {
            unsigned short position = (y + row) * limit_x + x + column;
            sum += (position & 3) == 1 ? position : 3;
            column += 1;
        }
        row += 1;
    }
    return sum;
}

static unsigned short live_outside(unsigned short seed, unsigned short count) {
    unsigned short a = seed + 1, b = seed + 2, c = seed + 3, d = seed + 4;
    unsigned short e = seed + 5, f = seed + 6, g = seed + 7, h = seed + 8;
    unsigned short i = seed + 9, j = seed + 10, k = seed + 11, l = seed + 12;
    for (unsigned short n = 0; n < count; ++n) seed += 1;
    return seed + a + b + c + d + e + f + g + h + i + j + k + l;
}

int main(void) {
    if (clipped_sum(1, 2, 9, 7, 16, 12, 37) == reference(1, 2, 9, 7, 16, 12, 37)) putchar('Y'); else putchar('N');
    if (clipped_sum(14, 10, 9, 7, 16, 12, 65000) == reference(14, 10, 9, 7, 16, 12, 65000)) putchar('Y'); else putchar('N');
    if (clipped_sum(1, 2, 0, 7, 16, 12, 37) == 37) putchar('Y'); else putchar('N');
    if (clipped_sum(1, 2, 9, 0, 16, 12, 37) == 37) putchar('Y'); else putchar('N');
    if (clipped_sum(17, 2, 9, 7, 16, 12, 37) == 37) putchar('Y'); else putchar('N');
    if (clipped_sum(1, 13, 9, 7, 16, 12, 37) == 37) putchar('Y'); else putchar('N');
    if (live_outside(71, 19) == 1020) putchar('Y'); else putchar('N');
    if (live_outside(5000, 0) == 65078) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
