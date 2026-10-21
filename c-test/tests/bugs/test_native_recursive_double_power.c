#include <stdio.h>
static double power(double x, int n) {
    double v;
    if (n <= 0) return 1;
    v = power(x, n / 2);
    if (n % 2 == 0) return v * v;
    return x * v * v;
}
int main(void) {
    volatile double x = 2.0;
    putchar(power(x, 10) == 1024.0 ? 'Y' : 'N');
    putchar(power(x, 3) == 8.0 ? 'Y' : 'N');
    putchar('\n');
    return 0;
}
