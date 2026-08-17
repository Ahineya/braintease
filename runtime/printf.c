#include <stdarg.h>

void putchar(int c);

static int print_char(char c) {
    putchar((int)c);
    return 1;
}

static int print_str(char *s) {
    int n;

    n = 0;
    if (!s) {
        return 0;
    }
    while (*s) {
        putchar((int)*s);
        s = s + 1;
        n = n + 1;
    }
    return n;
}

static int digit_char(unsigned int d) {
    if (d < 10) {
        return (int)'0' + (int)d;
    }
    return (int)'a' + (int)(d - 10);
}

static int print_uint(unsigned int n, unsigned int base) {
    char buf[16];
    int i;
    int count;

    i = 0;
    count = 0;

    if (n == 0) {
        return print_char('0');
    }

    while (n > 0) {
        buf[i] = (char)digit_char(n % base);
        i = i + 1;
        n = n / base;
    }
    while (i) {
        i = i - 1;
        print_char(buf[i]);
        count = count + 1;
    }
    return count;
}

static int print_int(int n) {
    int count;
    unsigned int mag;

    if (n < 0) {
        print_char('-');
        mag = (unsigned int)0 - (unsigned int)n;
        return 1 + print_uint(mag, 10);
    }
    count = print_uint((unsigned int)n, 10);
    return count;
}

int printf(char *fmt, ...) {
    va_list ap;
    char *p;
    int count;

    va_start(ap, fmt);
    p = fmt;
    count = 0;

    while (*p) {
        if (*p == '%') {
            p = p + 1;
            if (*p == 0) {
                count = count + print_char('%');
                break;
            }
            switch (*p) {
            case 's':
                count = count + print_str(va_arg(ap, char *));
                break;
            case 'c':
                count = count + print_char((char)va_arg(ap, int));
                break;
            case 'd':
                count = count + print_int(va_arg(ap, int));
                break;
            case 'u':
                count = count + print_uint(va_arg(ap, unsigned int), 10);
                break;
            case 'x':
                count = count + print_uint(va_arg(ap, unsigned int), 16);
                break;
            case '%':
                count = count + print_char('%');
                break;
            default:
                count = count + print_char('%');
                count = count + print_char(*p);
                break;
            }
        } else {
            count = count + print_char(*p);
        }
        p = p + 1;
    }

    va_end(ap);
    return count;
}
