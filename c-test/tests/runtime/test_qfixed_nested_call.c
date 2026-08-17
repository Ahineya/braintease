#include "qfixed.h"

void putchar(int c);

int main() {
    q16_16_t a = q_from_int(1);
    q16_16_t b = Q16_16_HALF;
    q16_16_t c = q_add(a, q_mul(a, b));
    if (c.integer == 1 && c.frac == 0x8000) {
        putchar('Y');
    } else {
        putchar('N');
    }
    putchar('\n');
    return 0;
}
