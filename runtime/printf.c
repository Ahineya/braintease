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

static int digit_char(unsigned int d, int upper) {
    if (d < 10) {
        return (int)'0' + (int)d;
    }
    if (upper) {
        return (int)'A' + (int)(d - 10);
    }
    return (int)'a' + (int)(d - 10);
}

static int print_uint(unsigned int n, unsigned int base, int upper) {
    char buf[16];
    int i;
    int count;

    i = 0;
    count = 0;

    if (n == 0) {
        return print_char('0');
    }

    while (n > 0) {
        buf[i] = (char)digit_char(n % base, upper);
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
    unsigned int mag;

    if (n < 0) {
        print_char('-');
        mag = (unsigned int)0 - (unsigned int)n;
        return 1 + print_uint(mag, 10, 0);
    }
    return print_uint((unsigned int)n, 10, 0);
}

static int print_ulong(unsigned long n, unsigned int base, int upper) {
    char buf[32];
    int i;
    int count;
    unsigned long b;

    b = (unsigned long)base;
    i = 0;
    count = 0;

    if (n == 0) {
        return print_char('0');
    }

    while (n > 0) {
        buf[i] = (char)digit_char((unsigned int)(n % b), upper);
        i = i + 1;
        n = n / b;
    }
    while (i) {
        i = i - 1;
        print_char(buf[i]);
        count = count + 1;
    }
    return count;
}

static int print_long(long n) {
    unsigned long mag;

    if (n < 0) {
        print_char('-');
        mag = 0UL - (unsigned long)n;
        return 1 + print_ulong(mag, 10, 0);
    }
    return print_ulong((unsigned long)n, 10, 0);
}

static int print_ulonglong(unsigned long long n, unsigned int base, int upper) {
    char buf[64];
    int i;
    int count;
    unsigned long long b;

    b = (unsigned long long)base;
    i = 0;
    count = 0;

    if (n == 0) {
        return print_char('0');
    }

    while (n > 0) {
        buf[i] = (char)digit_char((unsigned int)(n % b), upper);
        i = i + 1;
        n = n / b;
    }
    while (i) {
        i = i - 1;
        print_char(buf[i]);
        count = count + 1;
    }
    return count;
}

static int print_longlong(long long n) {
    unsigned long long mag;

    if (n < 0) {
        print_char('-');
        mag = 0ULL - (unsigned long long)n;
        return 1 + print_ulonglong(mag, 10, 0);
    }
    return print_ulonglong((unsigned long long)n, 10, 0);
}

int printf(char *fmt, ...) {
    va_list ap;
    char *p;
    int count;
    int long_mod;

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
            long_mod = 0;
            if (*p == 'l') {
                long_mod = 1;
                p = p + 1;
                if (*p == 0) {
                    count = count + print_char('%');
                    count = count + print_char('l');
                    break;
                }
                if (*p == 'l') {
                    long_mod = 2;
                    p = p + 1;
                    if (*p == 0) {
                        count = count + print_char('%');
                        count = count + print_char('l');
                        count = count + print_char('l');
                        break;
                    }
                }
            }
            switch (*p) {
            case 's':
                count = count + print_str(va_arg(ap, char *));
                break;
            case 'c':
                count = count + print_char((char)va_arg(ap, int));
                break;
            case 'd':
            case 'i':
                if (long_mod == 2) {
                    count = count + print_longlong(va_arg(ap, long long));
                } else if (long_mod) {
                    count = count + print_long(va_arg(ap, long));
                } else {
                    count = count + print_int(va_arg(ap, int));
                }
                break;
            case 'u':
                if (long_mod == 2) {
                    count = count + print_ulonglong(va_arg(ap, unsigned long long), 10, 0);
                } else if (long_mod) {
                    count = count + print_ulong(va_arg(ap, unsigned long), 10, 0);
                } else {
                    count = count + print_uint(va_arg(ap, unsigned int), 10, 0);
                }
                break;
            case 'X':
                if (long_mod == 2) {
                    count = count + print_ulonglong(va_arg(ap, unsigned long long), 16, 1);
                } else if (long_mod) {
                    count = count + print_ulong(va_arg(ap, unsigned long), 16, 1);
                } else {
                    count = count + print_uint(va_arg(ap, unsigned int), 16, 1);
                }
                break;
            case 'x':
                if (long_mod == 2) {
                    count = count + print_ulonglong(va_arg(ap, unsigned long long), 16, 0);
                } else if (long_mod) {
                    count = count + print_ulong(va_arg(ap, unsigned long), 16, 0);
                } else {
                    count = count + print_uint(va_arg(ap, unsigned int), 16, 0);
                }
                break;
            case '%':
                count = count + print_char('%');
                break;
            default:
                count = count + print_char('%');
                if (long_mod == 2) {
                    count = count + print_char('l');
                    count = count + print_char('l');
                } else if (long_mod) {
                    count = count + print_char('l');
                }
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
