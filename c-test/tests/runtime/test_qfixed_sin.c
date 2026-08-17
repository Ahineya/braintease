#include "qfixed.h"

void putchar(int c);

int main() {
    q16_16_t z = Q16_16_ZERO;
    q16_16_t s = q_sin(z);
    if (s.integer == 0 && s.frac < 0x1000) {
        putchar('Y');
    } else {
        putchar('N');
    }
    putchar('\n');
    return 0;
}
