#include <stdio.h>
#include <stdarg.h>

static void take_double(char *fmt, ...) {
    va_list ap;
    double x;
    va_start(ap, fmt);
    x = va_arg(ap, double);
    va_end(ap);
    if (x == 1.5) putchar('Y'); else putchar('N');
}

static void store_double(char *fmt, ...) {
    va_list ap;
    double *p;
    va_start(ap, fmt);
    p = va_arg(ap, double *);
    *p = 2.5;
    va_end(ap);
}

int main() {
    double d;

    take_double("%f", 1.5);

    d = 0.0;
    store_double("%lf", &d);
    if (d == 2.5) putchar('Y'); else putchar('N');

    putchar('\n');
    return 0;
}
