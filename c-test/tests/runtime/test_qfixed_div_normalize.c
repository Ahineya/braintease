#include "qfixed.h"

void putchar(int c);

int main() {
    q16_16_t three = q_from_int(3);
    q16_16_t four = q_from_int(4);
    q16_16_t five = q_from_int(5);

    q16_16_t x = q_div(three, five);
    q16_16_t y = q_div(four, five);

    if (x.integer == 0 && x.frac > 0x8000 && x.frac < 0xB000) {
        putchar('A');
    } else {
        putchar('a');
    }

    if (y.integer == 0 && y.frac > 0xB000 && y.frac < 0xE000) {
        putchar('B');
    } else {
        putchar('b');
    }

    q16_16_t mag = q_sqrt(q_add(q_mul(x, x), q_mul(y, y)));
    q16_16_t err = q_abs(q_sub(mag, Q16_16_ONE));
    if (err.integer == 0 && err.frac < 0x1000) {
        putchar('C');
    } else {
        putchar('c');
    }

    putchar('\n');
    return 0;
}
