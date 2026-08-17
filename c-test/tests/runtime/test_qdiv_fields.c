/* Runtime q_div, one check per field, so a wrong quotient is not a single N. */
#include "qfixed.h"

void putchar(int c);

static void yn(int c) {
    putchar(c ? 'Y' : 'N');
}

int main() {
    q16_16_t a;
    q16_16_t b;
    q16_16_t r;

    a = q_from_int(10);
    b = q_from_int(4);
    r = q_div(a, b);
    yn(r.integer == 2);
    yn(r.frac == 0x8000);

    a = q_from_int(3);
    b = q_from_int(5);
    r = q_div(a, b);
    yn(r.integer == 0);
    yn(r.frac > 0x8000 && r.frac < 0xB000);

    a = q_from_int(4);
    b = q_from_int(5);
    r = q_div(a, b);
    yn(r.integer == 0);
    yn(r.frac > 0xB000 && r.frac < 0xE000);

    a = q_from_int(1);
    b = q_from_int(2);
    r = q_div(a, b);
    yn(r.integer == 0);
    yn(r.frac == 0x8000);

    putchar('\n');
    return 0;
}
