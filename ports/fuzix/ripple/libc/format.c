/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Integer/string printf for the initial Unix utilities. No floating formatter.
 */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#ifdef RIPPLE_FLOAT_FORMAT
#include "../float-format.inc"
#endif
struct output {
    FILE *f;
    char *s;
    size_t cap, count;
    int failed;
};
static void emit(struct output *o, int c) {
    if (o->f) {
        if (fputc(c, o->f) == EOF)
            o->failed = 1;
    } else if (o->cap && o->count < o->cap - 1)
        o->s[o->count] = c;
    o->count++;
}
static int format(struct output *o, const char *fmt, va_list ap) {
    while (*fmt) {
        int left = 0, zero = 0, plus = 0, space = 0, hash = 0, width = 0,
            prec = -1, wide = 0, base = 10, upper = 0;
        char digits[34], prefix[3], *s;
#ifdef RIPPLE_FLOAT_FORMAT
        char floating[400];
#endif
        size_t n, i, np = 0, z = 0;
        unsigned long value = 0;
        long signed_value;
        if (*fmt != '%') {
            emit(o, *fmt++);
            continue;
        }
        fmt++;
        for (;; fmt++) {
            if (*fmt == '-')
                left = 1;
            else if (*fmt == '0')
                zero = 1;
            else if (*fmt == '+')
                plus = 1;
            else if (*fmt == ' ')
                space = 1;
            else if (*fmt == '#')
                hash = 1;
            else
                break;
        }
        if (*fmt == '*') {
            width = va_arg(ap, int);
            fmt++;
            if (width < 0) {
                left = 1;
                width = -width;
            }
        } else
            while (*fmt >= '0' && *fmt <= '9') {
                if (width < 3000)
                    width = width * 10 + *fmt - '0';
                fmt++;
            }
        if (*fmt == '.') {
            prec = 0;
            fmt++;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                fmt++;
            } else
                while (*fmt >= '0' && *fmt <= '9') {
                    if (prec < 3000)
                        prec = prec * 10 + *fmt - '0';
                    fmt++;
                }
        }
        if (*fmt == 'l') {
            wide = 1;
            fmt++;
        } else if (*fmt == 'z') {
            wide = 1;
            fmt++;
        } else if (*fmt == 'h') {
            fmt++;
            if (*fmt == 'h')
                fmt++;
        }
        if (*fmt == 's') {
            s = va_arg(ap, char *);
            if (!s)
                s = "(null)";
            n = strlen(s);
            if (prec >= 0 && n > (unsigned)prec)
                n = prec;
        } else if (*fmt == 'c') {
            digits[0] = va_arg(ap, int);
            s = digits;
            n = 1;
        } else if (*fmt == '%') {
            digits[0] = '%';
            s = digits;
            n = 1;
        } else if (strchr("diuoxXp", *fmt) && *fmt) {
            if (*fmt == 'd' || *fmt == 'i') {
                signed_value = wide ? va_arg(ap, long) : va_arg(ap, int);
                if (signed_value < 0) {
                    prefix[np++] = '-';
                    value = 0UL - (unsigned long)signed_value;
                } else {
                    value = signed_value;
                    if (plus)
                        prefix[np++] = '+';
                    else if (space)
                        prefix[np++] = ' ';
                }
            } else if (*fmt == 'p') {
                value = (unsigned long)va_arg(ap, void *);
                hash = 1;
                base = 16;
            } else {
                value = wide ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
                if (*fmt == 'o')
                    base = 8;
                else if (*fmt == 'x' || *fmt == 'X')
                    base = 16;
            }
            upper = *fmt == 'X';
            s = digits + sizeof(digits);
            n = 0;
            if (hash && base == 16 && value) {
                prefix[np++] = '0';
                prefix[np++] = upper ? 'X' : 'x';
            }
            if (value || prec != 0)
                do {
                    unsigned d = value % base;
                    *--s = d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10;
                    n++;
                    value /= base;
                } while (value);
            if (prec > 0 && (unsigned)prec > n)
                z = prec - n;
            if (hash && base == 8 && (!n || *s != '0') && !z)
                z = 1;
            if (zero && !left && prec < 0 && width > 0 &&
                (unsigned)width > np + n)
                z = width - np - n;
#ifdef RIPPLE_FLOAT_FORMAT
        } else if (*fmt && strchr("fFeEgG", *fmt)) {
            double number = va_arg(ap, double);
            if (signbit(number)) { prefix[np++] = '-'; number = -number; }
            else if (plus) prefix[np++] = '+';
            else if (space) prefix[np++] = ' ';
            n = float_text(floating, number, prec, *fmt, hash);
            s = floating;
            if (zero && !left && width > 0 && (unsigned)width > np + n)
                z = width - np - n;
#endif
        } else {
            errno = EINVAL;
            return -1;
        }
        if (!*fmt) {
            errno = EINVAL;
            return -1;
        }
        fmt++;
        if (!left)
            while (width > 0 && (unsigned)width > np + n + z) {
                emit(o, ' ');
                width--;
            }
        for (i = 0; i < np; i++)
            emit(o, prefix[i]);
        for (i = 0; i < z; i++)
            emit(o, '0');
        for (i = 0; i < n; i++)
            emit(o, s[i]);
        if (left)
            while (width > 0 && (unsigned)width > np + n + z) {
                emit(o, ' ');
                width--;
            }
    }
    if (!o->f && o->cap)
        o->s[o->count < o->cap ? o->count : o->cap - 1] = 0;
    if (o->count > INT_MAX) {
        errno = ERANGE;
        return -1;
    }
    return o->failed ? -1 : (int)o->count;
}
int vfprintf(FILE *f, const char *fmt, va_list a) {
    struct output o = {f, NULL, 0, 0, 0};
    return format(&o, fmt, a);
}
int vprintf(const char *fmt, va_list a) { return vfprintf(stdout, fmt, a); }
int vsnprintf(char *s, size_t n, const char *fmt, va_list a) {
    struct output o = {NULL, s, n, 0, 0};
    return format(&o, fmt, a);
}
int vsprintf(char *s, const char *fmt, va_list a) {
    return vsnprintf(s, ~0UL, fmt, a);
}
int fprintf(FILE *f, const char *fmt, ...) {
    va_list a;
    int n;
    va_start(a, fmt);
    n = vfprintf(f, fmt, a);
    va_end(a);
    return n;
}
int printf(const char *fmt, ...) {
    va_list a;
    int n;
    va_start(a, fmt);
    n = vprintf(fmt, a);
    va_end(a);
    return n;
}
int snprintf(char *s, size_t cap, const char *fmt, ...) {
    va_list a;
    int n;
    va_start(a, fmt);
    n = vsnprintf(s, cap, fmt, a);
    va_end(a);
    return n;
}
int sprintf(char *s, const char *fmt, ...) {
    va_list a;
    int n;
    va_start(a, fmt);
    n = vsprintf(s, fmt, a);
    va_end(a);
    return n;
}
