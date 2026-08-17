#include "qfixed.h"

void putchar(int c);

int main() {
    q16_16_t c = q_cos(Q16_16_ZERO);
    if (c.integer == 1 && c.frac < 0x1000) {
        putchar('Y');
    } else {
        putchar('N');
    }
    putchar('\n');
    return 0;
}
