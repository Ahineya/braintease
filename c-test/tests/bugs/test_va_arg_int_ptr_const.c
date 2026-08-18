/* Narrower than test_va_arg_int_ptr_busy: no ulong helper, store the
 * constant 42. Sibling char* / long* va_arg branches still present.
 */
#include <stdio.h>
#include <stdarg.h>

static va_list g_ap;

static int busy_fmt(char *fmt) {
    int assigned;
    int long_mod;
    char *dp;
    int *ip;
    long *lp;

    assigned = 0;
    long_mod = 0;
    dp = (char *)0;
    ip = (int *)0;
    lp = (long *)0;

    while (*fmt) {
        if (*fmt == '%') {
            fmt = fmt + 1;
            if (*fmt == 'l') {
                long_mod = 1;
                fmt = fmt + 1;
            }
            if (*fmt == 'c') {
                dp = va_arg(g_ap, char *);
                *dp = 'A';
                assigned = assigned + 1;
            } else if (*fmt == 'd') {
                if (long_mod == 1) {
                    lp = va_arg(g_ap, long *);
                    *lp = 42L;
                } else {
                    ip = va_arg(g_ap, int *);
                    *ip = 42;
                }
                assigned = assigned + 1;
            }
        }
        fmt = fmt + 1;
        long_mod = 0;
    }
    return assigned;
}

static int apply(char *fmt, ...) {
    int n;
    va_start(g_ap, fmt);
    n = busy_fmt(fmt);
    va_end(g_ap);
    return n;
}

int main() {
    int d;
    int n;

    d = 0;
    n = apply("%d", &d);
    if (n == 1 && d == 42) putchar('Y'); else putchar('N');

    putchar('\n');
    return 0;
}
