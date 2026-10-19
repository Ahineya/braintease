/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "user.h"
#include <stdarg.h>
#include <__banked_memory.h>


int ripple_error, errno;
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
        ripple_error = errno = r.error;

        return -1;
    }
    return v;
}
int open(const char *s, int f, ...) {
    unsigned m = 0;
    va_list a;
    if (f & 256) {
        va_start(a, f);
        m = va_arg(a, unsigned);
        va_end(a);
    }
    return (int)call(1, (long)s, f, m, 0);
}
int close(int f) { return (int)call(2, f, 0, 0, 0); }
long read(int f, void *p, unsigned long n) { return call(7, f, (long)p, n, 0); }
long write(int f, const void *p, unsigned long n) {
    return call(8, f, (long)p, n, 0);
}
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
size_t strlen(const char *s) { return __rcc_banked_length(s); }
int strcmp(const char *a, const char *b) { return __rcc_banked_compare(a, b); }
void *memcpy(void *d, const void *s, size_t n) {
    return __rcc_banked_copy(d, s, n);
}
void *memset(void *d, int c, size_t n) { return __rcc_banked_set(d, c, n); }
void rbf_user_exit(unsigned code) { call(0,code,0,0,0); }
void user_exit(void) { rbf_user_exit(0); }

void exit(int code) { rbf_user_exit(code); }
void perror(const char *name) {
    if (name && *name) { write(2,name,strlen(name)); write(2,": ",2); }
    /* Keep this small library independent of stdio buffers. */
    {
        char b[6]; unsigned n=errno,i=0;
        do { b[i++]='0'+n%10; n/=10; } while (n);
        write(2,"error ",6);
        while (i) write(2,&b[--i],1);
        write(2,"\n",1);
    }
}
