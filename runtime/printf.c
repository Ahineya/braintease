#include <stdarg.h>
#include <mmio.h>

void putchar(int c);

struct console_output { char data[256]; unsigned count; };
static void flush_output(struct console_output *o) {
    if (o->count) tty_write_bytes(o->data, o->count);
    o->count = 0;
}

static int print_char(struct console_output *o, char c) {
    /* Legacy putchar owns host raw-terminal newline translation. */
    if (c == '\n') { flush_output(o); putchar(c); return 1; }
    o->data[o->count++] = c;
    if (o->count == sizeof(o->data)) flush_output(o);
    return 1;
}

static int print_str(struct console_output *o, char *s) {
    int n;

    n = 0;
    if (!s) {
        return 0;
    }
    while (*s) {
        print_char(o, *s);
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

static int print_uint(struct console_output *o, unsigned int n, unsigned int base, int upper) {
    char buf[16];
    int i;
    int count;

    i = 0;
    count = 0;

    if (n == 0) {
        return print_char(o, '0');
    }

    while (n > 0) {
        buf[i] = (char)digit_char(n % base, upper);
        i = i + 1;
        n = n / base;
    }
    while (i) {
        i = i - 1;
        print_char(o, buf[i]);
        count = count + 1;
    }
    return count;
}

static int print_int(struct console_output *o, int n) {
    unsigned int mag;

    if (n < 0) {
        print_char(o, '-');
        mag = (unsigned int)0 - (unsigned int)n;
        return 1 + print_uint(o, mag, 10, 0);
    }
    return print_uint(o, (unsigned int)n, 10, 0);
}

static int print_ulong(struct console_output *o, unsigned long n, unsigned int base, int upper) {
    char buf[32];
    int i;
    int count;
    unsigned long b;

    b = (unsigned long)base;
    i = 0;
    count = 0;

    if (n == 0) {
        return print_char(o, '0');
    }

    while (n > 0) {
        buf[i] = (char)digit_char((unsigned int)(n % b), upper);
        i = i + 1;
        n = n / b;
    }
    while (i) {
        i = i - 1;
        print_char(o, buf[i]);
        count = count + 1;
    }
    return count;
}

static int print_long(struct console_output *o, long n) {
    unsigned long mag;

    if (n < 0) {
        print_char(o, '-');
        mag = 0UL - (unsigned long)n;
        return 1 + print_ulong(o, mag, 10, 0);
    }
    return print_ulong(o, (unsigned long)n, 10, 0);
}

static int print_ulonglong(struct console_output *o, unsigned long long n, unsigned int base, int upper) {
    char buf[64];
    int i;
    int count;
    unsigned long long b;

    b = (unsigned long long)base;
    i = 0;
    count = 0;

    if (n == 0) {
        return print_char(o, '0');
    }

    while (n > 0) {
        buf[i] = (char)digit_char((unsigned int)(n % b), upper);
        i = i + 1;
        n = n / b;
    }
    while (i) {
        i = i - 1;
        print_char(o, buf[i]);
        count = count + 1;
    }
    return count;
}

static int print_longlong(struct console_output *o, long long n) {
    unsigned long long mag;

    if (n < 0) {
        print_char(o, '-');
        mag = 0ULL - (unsigned long long)n;
        return 1 + print_ulonglong(o, mag, 10, 0);
    }
    return print_ulonglong(o, (unsigned long long)n, 10, 0);
}

static int print_double(struct console_output *o, double x, int prec) {
    long long ip;
    double frac;
    double scale;
    int i;
    int n;
    int d;

    if (x != x) {
        return print_str(o, "nan");
    }
    n = 0;
    if (x < 0.0) {
        n = n + print_char(o, '-');
        x = 0.0 - x;
    }
    if (x != 0.0 && x + x == x) {
        return n + print_str(o, "inf");
    }
    if (prec < 0) {
        prec = 6;
    }
    if (prec > 12) {
        prec = 12;
    }

    scale = 1.0;
    i = 0;
    while (i < prec) {
        scale = scale * 10.0;
        i = i + 1;
    }
    if (prec > 0) {
        x = x + 0.5 / scale;
    } else {
        x = x + 0.5;
    }

    ip = (long long)x;
    frac = x - (double)ip;
    if (frac < 0.0) {
        frac = 0.0;
    }
    if (frac >= 1.0) {
        ip = ip + 1;
        frac = frac - 1.0;
    }

    n = n + print_longlong(o, ip);
    if (prec > 0) {
        n = n + print_char(o, '.');
        i = 0;
        while (i < prec) {
            frac = frac * 10.0;
            d = (int)frac;
            if (d < 0) {
                d = 0;
            }
            if (d > 9) {
                d = 9;
            }
            n = n + print_char(o, (char)('0' + d));
            frac = frac - (double)d;
            i = i + 1;
        }
    }
    return n;
}

int printf(char *fmt, ...) {
    struct console_output storage;
    struct console_output *o = &storage;
    va_list ap;
    char *p;
    int count;
    int long_mod;
    int prec;

    o->count = 0;
    va_start(ap, fmt);
    p = fmt;
    count = 0;

    while (*p) {
        if (*p == '%') {
            p = p + 1;
            if (*p == 0) {
                count = count + print_char(o, '%');
                break;
            }
            while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0') {
                p = p + 1;
                if (*p == 0) {
                    break;
                }
            }
            while (*p >= '0' && *p <= '9') {
                p = p + 1;
                if (*p == 0) {
                    break;
                }
            }
            prec = -1;
            if (*p == '.') {
                p = p + 1;
                prec = 0;
                if (*p == 0) {
                    count = count + print_char(o, '%');
                    break;
                }
                while (*p >= '0' && *p <= '9') {
                    prec = prec * 10 + (*p - '0');
                    p = p + 1;
                    if (*p == 0) {
                        break;
                    }
                }
            }
            if (*p == 0) {
                count = count + print_char(o, '%');
                break;
            }
            long_mod = 0;
            if (*p == 'l') {
                long_mod = 1;
                p = p + 1;
                if (*p == 0) {
                    count = count + print_char(o, '%');
                    count = count + print_char(o, 'l');
                    break;
                }
                if (*p == 'l') {
                    long_mod = 2;
                    p = p + 1;
                    if (*p == 0) {
                        count = count + print_char(o, '%');
                        count = count + print_char(o, 'l');
                        count = count + print_char(o, 'l');
                        break;
                    }
                }
            }
            switch (*p) {
            case 's':
                count = count + print_str(o, va_arg(ap, char *));
                break;
            case 'c':
                count = count + print_char(o, (char)va_arg(ap, int));
                break;
            case 'd':
            case 'i':
                if (long_mod == 2) {
                    count = count + print_longlong(o, va_arg(ap, long long));
                } else if (long_mod) {
                    count = count + print_long(o, va_arg(ap, long));
                } else {
                    count = count + print_int(o, va_arg(ap, int));
                }
                break;
            case 'u':
                if (long_mod == 2) {
                    count = count + print_ulonglong(o, va_arg(ap, unsigned long long), 10, 0);
                } else if (long_mod) {
                    count = count + print_ulong(o, va_arg(ap, unsigned long), 10, 0);
                } else {
                    count = count + print_uint(o, va_arg(ap, unsigned int), 10, 0);
                }
                break;
            case 'X':
                if (long_mod == 2) {
                    count = count + print_ulonglong(o, va_arg(ap, unsigned long long), 16, 1);
                } else if (long_mod) {
                    count = count + print_ulong(o, va_arg(ap, unsigned long), 16, 1);
                } else {
                    count = count + print_uint(o, va_arg(ap, unsigned int), 16, 1);
                }
                break;
            case 'x':
                if (long_mod == 2) {
                    count = count + print_ulonglong(o, va_arg(ap, unsigned long long), 16, 0);
                } else if (long_mod) {
                    count = count + print_ulong(o, va_arg(ap, unsigned long), 16, 0);
                } else {
                    count = count + print_uint(o, va_arg(ap, unsigned int), 16, 0);
                }
                break;
            case '%':
                count = count + print_char(o, '%');
                break;
            case 'f':
            case 'F':
                count = count + print_double(o, va_arg(ap, double), prec);
                break;
            default:
                count = count + print_char(o, '%');
                if (long_mod == 2) {
                    count = count + print_char(o, 'l');
                    count = count + print_char(o, 'l');
                } else if (long_mod) {
                    count = count + print_char(o, 'l');
                }
                count = count + print_char(o, *p);
                break;
            }
        } else {
            count = count + print_char(o, *p);
        }
        p = p + 1;
    }

    va_end(ap);
    flush_output(o);
    return count;
}
