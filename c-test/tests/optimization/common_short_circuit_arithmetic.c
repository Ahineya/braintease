#include <stdio.h>

static unsigned short width = 7;
static unsigned short height = 5;
static volatile unsigned short input = 2;
static volatile unsigned short rhs_calls = 0;

static unsigned short pixel(unsigned short x, unsigned short y) {
    if (x >= width || y >= height) return 5000;
    return y * width + x + 1;
}

static unsigned short rectangle(unsigned short x, unsigned short y,
                                unsigned short w, unsigned short h) {
    unsigned short sum = 0;
    for (unsigned short dy = 0; dy < h && (y + dy) < height; ++dy)
        for (unsigned short dx = 0; dx < w && (x + dx) < width; ++dx)
            sum += pixel(x + dx, y + dy);
    return sum;
}

static unsigned short guarded_sum(unsigned short base, unsigned short delta) {
    if (delta < 8 && (base + delta) < 7) {
        if ((base + delta) >= 7) return 99;
        return base + delta;
    }
    return 123;
}

static unsigned short guarded_division(unsigned short divisor, unsigned short flag) {
    if (divisor != 0 && flag != 0) return 20 / divisor;
    if (divisor != 0 && flag == 0) return 20 / divisor;
    return 77;
}

int main(void) {
    if (rectangle(0, 0, 7, 5) == 630) putchar('Y'); else putchar('N');
    if (rectangle(input, 1, 3, 2) == 87) putchar('Y'); else putchar('N');
    if (rectangle(6, 4, 4, 4) == 35) putchar('Y'); else putchar('N');
    if (rectangle(7, 0, 3, 2) == 0 && rectangle(0, 5, 3, 2) == 0) putchar('Y'); else putchar('N');
    if (rectangle(0, 0, 0, 5) == 0 && rectangle(0, 0, 7, 0) == 0) putchar('Y'); else putchar('N');
    if (guarded_sum(65535, input) == 1) putchar('Y'); else putchar('N');
    if (guarded_sum(5, input) == 123 && guarded_sum(65530, input) == 123) putchar('Y'); else putchar('N');
    if (guarded_division(0, input) == 77 && guarded_division(input, 0) == 10) putchar('Y'); else putchar('N');

    rhs_calls = 0;
    if (input == 0 && (++rhs_calls, (input + 3) < 7)) rhs_calls = 99;
    if (rhs_calls == 0) putchar('Y'); else putchar('N');
    if (input != 0 && (++rhs_calls, (input + 3) < 7)) {
        input = 8;
        if ((input + 3) < 7) rhs_calls = 99;
    }
    if (rhs_calls == 1) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
