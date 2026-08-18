#include <stdio.h>

int main() {
    double a;
    double b;
    double prod;
    float f;
    int n;

    n = sscanf("1.5 2.5", "%lf %lf", &a, &b);
    if (n == 2 && a == 1.5 && b == 2.5) putchar('Y'); else putchar('N');

    prod = a * b;
    if (prod == 3.75) putchar('Y'); else putchar('N');

    n = sscanf("12.5", "%f", &f);
    if (n == 1 && f == 12.5f) putchar('Y'); else putchar('N');

    n = sscanf("-2.5", "%lf", &a);
    if (n == 1 && a == -2.5) putchar('Y'); else putchar('N');

    n = sscanf("2e2", "%lf", &a);
    if (n == 1 && a == 200.0) putchar('Y'); else putchar('N');

    putchar('\n');
    return 0;
}
