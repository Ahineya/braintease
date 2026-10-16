/* SPDX-License-Identifier: GPL-2.0-only */
#include "user.h"
int error;
long call(unsigned n, long a, long b, long c, long d) {
    struct request r;
    long v;
    r.call = n;
    r.arg[0] = a;
    r.arg[1] = b;
    r.arg[2] = c;
    r.arg[3] = d;
    r.error = 0;
    v = ripple_syscall(&r);
    if (r.error) {
        error = r.error;
        return -1;
    }
    return v;
}
int open(const char *s, unsigned f, unsigned m) { return (int)call(1, (long)s, f, m, 0); }
int close(int f) { return (int)call(2, f, 0, 0, 0); }
long read(int f, void *p, unsigned long n) { return call(7, f, (long)p, n, 0); }
long write(int f, const void *p, unsigned long n) { return call(8, f, (long)p, n, 0); }
void out(const char *s) { write(1, s, strlen(s)); }
void number(unsigned n) {
    char b[6];
    unsigned i = 0;
    do {
        b[i++] = '0' + n % 10;
        n /= 10;
    } while (n);
    while (i)
        write(1, &b[--i], 1);
}
size_t strlen(const char *s) {
    const char *p = s;
    while (*p)
        p++;
    return p - s;
}
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned)*a - (unsigned)*b;
}
void *memcpy(void *d, const void *s, size_t n) {
    char *a = d;
    const char *b = s;
    while (n--)
        *a++ = *b++;
    return d;
}
void *memset(void *d, int c, size_t n) {
    char *a = d;
    while (n--)
        *a++ = c;
    return d;
}
void user_exit(void) {
    call(0, 0, 0, 0, 0);
    for (;;)
        ;
}
