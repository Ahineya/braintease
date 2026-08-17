#include <stdarg.h>

void putchar(int c);

int sum(int n, ...) {
    va_list ap;
    int s;
    int i;
    va_start(ap, n);
    s = 0;
    for (i = 0; i < n; i = i + 1) {
        s = s + va_arg(ap, int);
    }
    va_end(ap);
    return s;
}

int main() {
    if (sum(3, 1, 2, 3) == 6) putchar('Y'); else putchar('N');
    if (sum(0) == 0) putchar('Y'); else putchar('N');
    if (sum(2, 10, 20) == 30) putchar('Y'); else putchar('N');
    if (sum(1, 42) == 42) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
