/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "user.h"
#include <stdarg.h>
#include <__banked_memory.h>
extern int errno;
extern void exit(int);
int ripple_error;
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
        ripple_error = r.error;
        errno = r.error;
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
void user_exit(void) { exit(0); }

#if !defined(FUSE_X_USER) && !defined(RIPPLE_MMIO_NO_INLINE)
#define VM_MATH_STACK
#endif
#include <vm_math.h>
#ifdef FUSE_X_USER
extern unsigned long long __rcc_host_math(unsigned, unsigned long long, unsigned long long);
#endif
unsigned __rcc_float_accel(unsigned op, unsigned long long a,
                           unsigned long long b, unsigned long long *result) {
#ifdef FUSE_X_USER
    *result = __rcc_host_math(op, a, b);
    return VM_DEVICE_OK;
#else
    volatile VmMathRequest packet;
    struct request r;
    long status;
    packet.version = VM_DEVICE_ABI_VERSION; packet.length = VM_MATH_CELLS;
    packet.operation = op; packet.reserved = 0;
    packet.a = a; packet.b = b; packet.result = 0;
#ifdef VM_MATH_STACK
    status = vm_math_stack(&packet);
    if (status == VM_DEVICE_OK) { *result = packet.result; return VM_DEVICE_OK; }
    packet.reserved = 0;
#endif
    r.call = 27; r.arg[0] = (long)&packet; r.arg[1] = RIPPLE_TIME_MATH;
    r.arg[2] = 0; r.arg[3] = 0; r.error = 0;
    status = ripple_syscall(&r);
    /* Optional acceleration must never change the caller's errno. */
    if (r.error || status < 0) return VM_DEVICE_UNSUPPORTED;
    if (status != VM_DEVICE_OK) return (unsigned)status;
    *result = packet.result;
    return VM_DEVICE_OK;
#endif
}
