/* SPDX-License-Identifier: GPL-2.0-or-later */
/* String scanning for the C16 userspace ABI. No host FILE/va_list layout leaks
 * here. Floating and wide-character conversions are deliberately unsupported
 * in this integer-only libc; they fail with EINVAL before reading an argument. */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

static int digit(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int in_set(const char *start, const char *end, int c) {
    while (start < end) {
        int first = *start++;
        if (start + 1 < end && *start == '-') {
            ++start;
            if (c >= first && c <= *start++) return 1;
        } else if (c == first) return 1;
    }
    return 0;
}
int vsscanf(const char *input, const char *fmt, va_list ap) {
    const char *s = input;
    int assignments = 0, converted = 0;
    while (*fmt) {
        unsigned long width = ULONG_MAX;
        int suppress = 0, length = 0, spec;
        if (isspace(*fmt)) {
            while (isspace(*fmt)) ++fmt;
            while (isspace(*s)) ++s;
            continue;
        }
        if (*fmt++ != '%') {
            if (!*s) return converted ? assignments : EOF;
            if (*s != fmt[-1]) return assignments;
            ++s; continue;
        }
        if (*fmt == '*') { suppress = 1; ++fmt; }
        if (isdigit(*fmt)) {
            width = 0;
            do {
                unsigned d = *fmt++ - '0';
                if (width > (ULONG_MAX - d) / 10) { errno = EINVAL; return assignments; }
                width = width * 10 + d;
            } while (isdigit(*fmt));
            if (!width) { errno = EINVAL; return assignments; }
        }
        if (*fmt == 'h') { length = -1; ++fmt; if (*fmt == 'h') { length = -2; ++fmt; } }
        else if (*fmt == 'l') { length = 1; ++fmt; }
        else if (*fmt == 'z' || *fmt == 't') { length = 1; ++fmt; }
        spec = *fmt++;
        if (spec == '%') {
            if (!*s) return converted ? assignments : EOF;
            if (*s != '%') return assignments;
            ++s; continue;
        }
        if (spec == 'n') {
            if (!suppress) {
                if (length == 1) *va_arg(ap, long *) = (long)(s - input);
                else *va_arg(ap, int *) = (int)(s - input);
            }
            converted = 1; continue;
        }
        if (spec != 'c' && spec != '[') while (isspace(*s)) ++s;
        if (spec == 'c' || spec == 's' || spec == '[') {
            const char *set = NULL, *end = NULL;
            char *out = NULL;
            unsigned long n = 0;
            int invert = 0;
            if (length == 1) { errno = EINVAL; return assignments; }
            if (spec == '[') {
                if (*fmt == '^') { invert = 1; ++fmt; }
                set = fmt;
                if (*fmt == ']') ++fmt;
                while (*fmt && *fmt != ']') ++fmt;
                if (!*fmt) { errno = EINVAL; return assignments; }
                end = fmt++;
            }
            if (!*s) return converted ? assignments : EOF;
            if (spec == 'c' && width == ULONG_MAX) width = 1;
            if (!suppress) out = va_arg(ap, char *);
            while (n < width && *s) {
                if (spec == 's' && isspace(*s)) break;
                if (spec == '[' && in_set(set, end, *s) == invert) break;
                if (out) out[n] = *s;
                ++s; ++n;
            }
            if (!n || (spec == 'c' && n != width)) return assignments;
            if (out && spec != 'c') out[n] = 0;
            if (!suppress) ++assignments;
            converted = 1; continue;
        }
        if (spec == 'd' || spec == 'i' || spec == 'u' || spec == 'o' || spec == 'x' || spec == 'X' || spec == 'p') {
            unsigned long n = 0, used = 0, limit;
            int negative = 0, base = spec == 'o' ? 8 : (spec == 'x' || spec == 'X' || spec == 'p' ? 16 : 10);
            int digits = 0, overflow = 0, d;
            int signed_conversion = spec == 'd' || spec == 'i';
            if (!*s) return converted ? assignments : EOF;
            if ((*s == '+' || *s == '-') && width) { negative = *s == '-'; ++s; ++used; }
            if (spec == 'i') base = *s == '0' ? 8 : 10;
            if ((spec == 'i' || base == 16) && used + 1 < width && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
                s += 2; used += 2; base = 16;
            }
            limit = (length == 1 || spec == 'p') ? ULONG_MAX : UINT_MAX;
            if (signed_conversion) limit = (length == 1 ? (unsigned long)LONG_MAX : (unsigned long)INT_MAX) + negative;
            while (used < width && (d = digit(*s)) >= 0 && d < base) {
                if (n > (limit - d) / base) overflow = 1;
                n = n * base + d;
                ++s; ++used; ++digits;
            }
            if (!digits) return assignments;
            if (overflow) { errno = ERANGE; return assignments; }
            if (negative) n = 0UL - n;
            if (!suppress) {
                if (spec == 'p') *va_arg(ap, void **) = (void *)n;
                else if (length == 1) {
                    if (signed_conversion) *va_arg(ap, long *) = (long)n;
                    else *va_arg(ap, unsigned long *) = n;
                } else if (signed_conversion) *va_arg(ap, int *) = (int)n;
                else *va_arg(ap, unsigned int *) = (unsigned int)n;
                ++assignments;
            }
            converted = 1; continue;
        }
        errno = EINVAL; return assignments;
    }
    return assignments;
}
int sscanf(const char *s, const char *fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt); n = vsscanf(s, fmt, ap); va_end(ap);
    return n;
}
