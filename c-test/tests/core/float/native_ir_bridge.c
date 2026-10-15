#include <stdio.h>
int main(void) {
    volatile float a = 1.25f, b = 2.5f;
    volatile double x = 3.125, y = 0.5;
    putchar(a + b == 3.75f ? 'Y' : 'N');
    putchar((x - y) * 2.0 / y == 10.5 ? 'Y' : 'N');
    putchar((long)(a * 10.0f) == 12L ? 'Y' : 'N');
    putchar((double)(unsigned long)4000000000UL == 4000000000.0 ? 'Y' : 'N');
    putchar((float)x == 3.125f && (double)a == 1.25 ? 'Y' : 'N');
    putchar('\n');
    return 0;
}
