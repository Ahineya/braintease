#include <stdio.h>
#include <stdarg.h>

static void store_va(char *kind, ...) {
    va_list ap;
    int *ip;
    char *cp;
    long *lp;
    int iv;

    va_start(ap, kind);
    iv = 42;
    if (*kind == 'i') {
        ip = va_arg(ap, int *);
        *ip = iv;
    } else if (*kind == 'c') {
        cp = va_arg(ap, char *);
        *cp = (char)iv;
    } else if (*kind == 'l') {
        lp = va_arg(ap, long *);
        *lp = (long)iv;
    } else if (*kind == 'C') {
        cp = va_arg(ap, char *);
        ip = (int *)cp;
        *ip = iv;
    }
    va_end(ap);
}

static void store_direct(int *p) {
    *p = 42;
}

int main() {
    int x;
    char c;
    long L;

    x = 0;
    store_direct(&x);
    if (x == 42) putchar('Y'); else putchar('N');

    x = 0;
    store_va("i", &x);
    if (x == 42) putchar('Y'); else putchar('N');

    c = 0;
    store_va("c", &c);
    if (c == 42) putchar('Y'); else putchar('N');

    L = 0;
    store_va("l", &L);
    if (L == 42L) putchar('Y'); else putchar('N');

    x = 0;
    store_va("C", &x);
    if (x == 42) putchar('Y'); else putchar('N');

    putchar('\n');
    return 0;
}
