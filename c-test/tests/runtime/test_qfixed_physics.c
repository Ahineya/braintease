#include "qfixed.h"

void putchar(int c);

int main() {
    q16_16_t x = q_from_int(0);
    q16_16_t y = q_from_int(100);
    q16_16_t vx = q_from_int(5);
    q16_16_t vy = q_from_int(0);
    q16_16_t gravity = q_from_int(-10);
    q16_16_t dt = (q16_16_t){0, 0x0666};

    for (int i = 0; i < 10; i++) {
        vy = q_add(vy, q_mul(gravity, dt));
        x = q_add(x, q_mul(vx, dt));
        y = q_add(y, q_mul(vy, dt));
    }

    if (q_gt(x, Q16_16_ZERO) && q_lt(y, q_from_int(100))) {
        putchar('Y');
    } else {
        putchar('N');
    }
    putchar('\n');
    return 0;
}
