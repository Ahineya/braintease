#include "qfixed.h"

void putchar(int c);

typedef struct {
    q16_16_t x, y;
    q16_16_t vx, vy;
} particle_t;

int main() {
    particle_t p;
    p.x = q_from_int(0);
    p.y = q_from_int(100);
    p.vx = q_from_int(5);
    p.vy = q_from_int(0);

    p.x = q_add(p.x, p.vx);
    p.y = q_add(p.y, p.vy);

    if (p.x.integer == 5 && p.y.integer == 100) {
        putchar('Y');
    } else {
        putchar('N');
    }
    putchar('\n');
    return 0;
}
