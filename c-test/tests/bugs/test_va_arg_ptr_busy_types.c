/* Same busy_fmt frame: char* and long* stores must work, int* must too.
 * In scanf, %c / %s / %ld succeeded in scan_fmt while %d / %x / %i wrote 0.
 */
#include <stdio.h>
#include <stdarg.h>

static va_list g_ap;
static unsigned long g_uval;
static unsigned int g_mag;

static int heavy_scan(void) {
    unsigned long val;
    unsigned int mag;
    int d;
    unsigned int b16;
    unsigned long b32;

    mag = 0;
    val = 0UL;
    b16 = 10;
    b32 = 10UL;
    d = 4;
    mag = mag * b16 + (unsigned int)d;
    val = val * b32 + (unsigned long)d;
    d = 2;
    mag = mag * b16 + (unsigned int)d;
    val = val * b32 + (unsigned long)d;
    g_mag = mag;
    g_uval = val;
    return 1;
}

static int busy_fmt(char *fmt) {
    int assigned;
    int long_mod;
    int c;
    int r;
    int iv;
    char *dp;
    int *ip;
    long *lp;
    long long *llp;

    assigned = 0;
    long_mod = 0;
    c = 0;
    r = 0;
    iv = 0;
    dp = (char *)0;
    ip = (int *)0;
    lp = (long *)0;
    llp = (long long *)0;

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
            } else if (*fmt == 's') {
                dp = va_arg(g_ap, char *);
                *dp = 'Z';
                dp = dp + 1;
                *dp = 0;
                assigned = assigned + 1;
            } else if (*fmt == 'd') {
                r = heavy_scan();
                if (long_mod == 1) {
                    lp = va_arg(g_ap, long *);
                    *lp = (long)g_uval;
                } else {
                    ip = va_arg(g_ap, int *);
                    iv = (int)g_mag;
                    *ip = iv;
                }
                assigned = assigned + 1;
            }
        }
        fmt = fmt + 1;
        long_mod = 0;
    }
    return assigned + c;
}

static int apply(char *fmt, ...) {
    int n;
    va_start(g_ap, fmt);
    n = busy_fmt(fmt);
    va_end(g_ap);
    return n;
}

int main() {
    char ch;
    char word[4];
    int d;
    long lv;
    int n;

    ch = 0;
    n = apply("%c", &ch);
    if (n >= 1 && ch == 'A') putchar('Y'); else putchar('N');

    word[0] = 0;
    n = apply("%s", word);
    if (n >= 1 && word[0] == 'Z' && word[1] == 0) putchar('Y'); else putchar('N');

    lv = 0;
    n = apply("%ld", &lv);
    if (n >= 1 && lv == 42L) putchar('Y'); else putchar('N');

    d = 0;
    n = apply("%d", &d);
    if (n >= 1 && d == 42) putchar('Y'); else putchar('N');

    putchar('\n');
    return 0;
}
