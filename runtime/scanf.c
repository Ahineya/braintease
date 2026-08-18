// Freestanding scanf / sscanf. Input for scanf is getchar() (TTY MMIO).
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <ctype.h>

struct ScanSrc {
    char *str;
    int pushback;
};

/* TTY scanf is line-buffered with echo (like a cooked terminal).
 * getchar() stays raw so Forth / tty_echo can echo themselves. */
static char g_line[128];
static int g_line_len;
static int g_line_pos;

static void tty_refill(void) {
    int c;

    g_line_len = 0;
    g_line_pos = 0;
    while (g_line_len < 127) {
        c = getchar();
        if (c == '\r') {
            c = '\n';
        }
        if (c == '\n') {
            putchar('\n');
            g_line[g_line_len] = '\n';
            g_line_len = g_line_len + 1;
            return;
        }
        if (c == 8 || c == 127) {
            if (g_line_len > 0) {
                g_line_len = g_line_len - 1;
                putchar(8);
                putchar(' ');
                putchar(8);
            }
            continue;
        }
        putchar(c);
        g_line[g_line_len] = (char)c;
        g_line_len = g_line_len + 1;
    }
    putchar('\n');
    g_line[g_line_len] = '\n';
    g_line_len = g_line_len + 1;
}

static int tty_getc(void) {
    int c;

    if (g_line_pos >= g_line_len) {
        tty_refill();
    }
    if (g_line_pos >= g_line_len) {
        return EOF;
    }
    c = (int)(unsigned char)g_line[g_line_pos];
    g_line_pos = g_line_pos + 1;
    return c;
}

static int src_getc(struct ScanSrc *src) {
    int c;

    if (src->pushback != EOF) {
        c = src->pushback;
        src->pushback = EOF;
        return c;
    }
    if (src->str) {
        if (*src->str == 0) {
            return EOF;
        }
        c = (int)(unsigned char)*src->str;
        src->str = src->str + 1;
        return c;
    }
    return tty_getc();
}

static void src_ungetc(struct ScanSrc *src, int c) {
    if (c != EOF) {
        src->pushback = c;
    }
}

static int skip_ws(struct ScanSrc *src) {
    int c;

    while (1) {
        c = src_getc(src);
        if (c == EOF) {
            return EOF;
        }
        if (!isspace(c)) {
            src_ungetc(src, c);
            return 0;
        }
    }
}

static int hex_val(int c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

static int digit_val(int c, int base) {
    int v;

    if (base == 16) {
        v = hex_val(c);
    } else if (c >= '0' && c <= '9') {
        v = c - '0';
    } else {
        v = -1;
    }
    if (v < 0 || v >= base) {
        return -1;
    }
    return v;
}

/* Magnitude and sign for 16-bit destinations; g_scan_uval for long. */
static unsigned int g_scan_mag;
static int g_scan_neg;
static unsigned long g_scan_uval;
static va_list g_ap;

/* Returns 1 on success, 0 on matching failure, -1 on EOF before a digit. */
static int scan_uint(struct ScanSrc *src, int base, int sign_ok) {
    int c;
    int neg;
    int digits;
    unsigned int mag;
    unsigned long val;
    int d;
    int auto_base;
    unsigned int b16;
    unsigned long b32;

    if (skip_ws(src) == EOF) {
        return -1;
    }

    neg = 0;
    c = src_getc(src);
    if (c == EOF) {
        return -1;
    }
    if (sign_ok && (c == '-' || c == '+')) {
        if (c == '-') {
            neg = 1;
        }
        c = src_getc(src);
        if (c == EOF) {
            return -1;
        }
    }

    auto_base = 0;
    if (base == 0) {
        auto_base = 1;
        base = 10;
        if (c == '0') {
            c = src_getc(src);
            if (c == 'x' || c == 'X') {
                base = 16;
                c = src_getc(src);
                if (c == EOF) {
                    return -1;
                }
            } else {
                base = 8;
                src_ungetc(src, c);
                c = '0';
            }
        }
    }

    mag = 0;
    val = 0UL;
    digits = 0;
    b16 = (unsigned int)base;
    b32 = (unsigned long)base;
    while (1) {
        d = digit_val(c, base);
        if (d < 0) {
            break;
        }
        mag = mag * b16 + (unsigned int)d;
        val = val * b32 + (unsigned long)d;
        digits = digits + 1;
        c = src_getc(src);
        if (c == EOF) {
            c = EOF;
            break;
        }
    }
    if (c != EOF) {
        src_ungetc(src, c);
    }

    if (digits == 0) {
        if (auto_base && base == 8) {
            g_scan_mag = 0;
            g_scan_neg = 0;
            g_scan_uval = 0UL;
            return 1;
        }
        return 0;
    }
    g_scan_mag = mag;
    g_scan_neg = neg;
    if (neg) {
        g_scan_uval = 0UL - val;
    } else {
        g_scan_uval = val;
    }
    return 1;
}

/* va_arg(int *) + store inside scan_fmt miscompiles; keep this tiny. */
static void store_int_arg(int v) {
    int *ip;
    ip = va_arg(g_ap, int *);
    *ip = v;
}

static void store_float_arg(float v) {
    float *fp;
    fp = va_arg(g_ap, float *);
    *fp = v;
}

static void store_double_arg(double v) {
    double *dp;
    dp = va_arg(g_ap, double *);
    *dp = v;
}

static double g_scan_dbl;

static int scan_double(struct ScanSrc *src) {
    int c;
    int neg;
    int saw;
    int exp_neg;
    int expv;
    int i;
    double val;
    double place;

    if (skip_ws(src) == EOF) {
        return -1;
    }

    neg = 0;
    c = src_getc(src);
    if (c == EOF) {
        return -1;
    }
    if (c == '-' || c == '+') {
        if (c == '-') {
            neg = 1;
        }
        c = src_getc(src);
        if (c == EOF) {
            return -1;
        }
    }

    val = 0.0;
    saw = 0;
    while (c >= '0' && c <= '9') {
        val = val * 10.0 + (double)(c - '0');
        saw = 1;
        c = src_getc(src);
        if (c == EOF) {
            c = EOF;
            break;
        }
    }
    if (c == '.') {
        c = src_getc(src);
        place = 0.1;
        while (c >= '0' && c <= '9') {
            val = val + (double)(c - '0') * place;
            place = place * 0.1;
            saw = 1;
            c = src_getc(src);
            if (c == EOF) {
                c = EOF;
                break;
            }
        }
    }
    if (c == 'e' || c == 'E') {
        exp_neg = 0;
        c = src_getc(src);
        if (c == '-' || c == '+') {
            if (c == '-') {
                exp_neg = 1;
            }
            c = src_getc(src);
        }
        expv = 0;
        i = 0;
        while (c >= '0' && c <= '9') {
            expv = expv * 10 + (c - '0');
            i = 1;
            c = src_getc(src);
            if (c == EOF) {
                c = EOF;
                break;
            }
        }
        if (i) {
            if (expv > 38) {
                expv = 38;
            }
            if (exp_neg) {
                i = 0;
                while (i < expv) {
                    val = val / 10.0;
                    i = i + 1;
                }
            } else {
                i = 0;
                while (i < expv) {
                    val = val * 10.0;
                    i = i + 1;
                }
            }
        }
    }
    if (c != EOF) {
        src_ungetc(src, c);
    }
    if (!saw) {
        return 0;
    }
    if (neg) {
        val = 0.0 - val;
    }
    g_scan_dbl = val;
    return 1;
}

static int scan_fmt(struct ScanSrc *src, char *fmt) {
    int assigned;
    int long_mod;
    int c;
    int r;
    int iv;
    char *dp;
    long *lp;
    long long *llp;

    assigned = 0;
    while (*fmt) {
        if (isspace((int)(unsigned char)*fmt)) {
            while (*fmt && isspace((int)(unsigned char)*fmt)) {
                fmt = fmt + 1;
            }
            skip_ws(src);
            continue;
        }

        if (*fmt != '%') {
            c = src_getc(src);
            if (c == EOF) {
                return assigned == 0 ? EOF : assigned;
            }
            if (c != (int)(unsigned char)*fmt) {
                src_ungetc(src, c);
                return assigned;
            }
            fmt = fmt + 1;
            continue;
        }

        fmt = fmt + 1;
        if (*fmt == 0) {
            break;
        }
        if (*fmt == '%') {
            c = src_getc(src);
            if (c == EOF) {
                return assigned == 0 ? EOF : assigned;
            }
            if (c != '%') {
                src_ungetc(src, c);
                return assigned;
            }
            fmt = fmt + 1;
            continue;
        }

        long_mod = 0;
        if (*fmt == 'l') {
            long_mod = 1;
            fmt = fmt + 1;
            if (*fmt == 0) {
                break;
            }
            if (*fmt == 'l') {
                long_mod = 2;
                fmt = fmt + 1;
                if (*fmt == 0) {
                    break;
                }
            }
        }

        if (*fmt == 'c') {
            c = src_getc(src);
            if (c == EOF) {
                return assigned == 0 ? EOF : assigned;
            }
            dp = va_arg(g_ap, char *);
            *dp = (char)c;
            assigned = assigned + 1;
        } else if (*fmt == 's') {
            if (skip_ws(src) == EOF) {
                return assigned == 0 ? EOF : assigned;
            }
            dp = va_arg(g_ap, char *);
            r = 0;
            while (1) {
                c = src_getc(src);
                if (c == EOF) {
                    break;
                }
                if (isspace(c)) {
                    src_ungetc(src, c);
                    break;
                }
                *dp = (char)c;
                dp = dp + 1;
                r = 1;
            }
            *dp = 0;
            if (!r) {
                return assigned == 0 ? EOF : assigned;
            }
            assigned = assigned + 1;
        } else if (*fmt == 'd' || *fmt == 'i' || *fmt == 'u' || *fmt == 'x' || *fmt == 'X') {
            r = 0;
            if (*fmt == 'd') {
                r = scan_uint(src, 10, 1);
            } else if (*fmt == 'i') {
                r = scan_uint(src, 0, 1);
            } else if (*fmt == 'u') {
                r = scan_uint(src, 10, 1);
            } else {
                r = scan_uint(src, 16, 1);
            }
            if (r < 0) {
                return assigned == 0 ? EOF : assigned;
            }
            if (r == 0) {
                return assigned;
            }
            if (long_mod == 2) {
                llp = va_arg(g_ap, long long *);
                *llp = (long long)g_scan_uval;
            } else if (long_mod == 1) {
                lp = va_arg(g_ap, long *);
                *lp = (long)g_scan_uval;
            } else {
                if (g_scan_neg) {
                    iv = 0 - (int)g_scan_mag;
                } else {
                    iv = (int)g_scan_mag;
                }
                store_int_arg(iv);
            }
            assigned = assigned + 1;
        } else if (*fmt == 'f' || *fmt == 'F' || *fmt == 'e' || *fmt == 'E' || *fmt == 'g' || *fmt == 'G') {
            r = scan_double(src);
            if (r < 0) {
                return assigned == 0 ? EOF : assigned;
            }
            if (r == 0) {
                return assigned;
            }
            if (long_mod == 1) {
                store_double_arg(g_scan_dbl);
            } else {
                store_float_arg((float)g_scan_dbl);
            }
            assigned = assigned + 1;
        } else {
            return assigned;
        }
        fmt = fmt + 1;
    }
    return assigned;
}

int scanf(char *fmt, ...) {
    struct ScanSrc src;
    int n;

    src.str = NULL;
    src.pushback = EOF;
    va_start(g_ap, fmt);
    n = scan_fmt(&src, fmt);
    va_end(g_ap);
    return n;
}

int sscanf(char *s, char *fmt, ...) {
    struct ScanSrc src;
    int n;

    if (!s) {
        return EOF;
    }
    src.str = s;
    src.pushback = EOF;
    va_start(g_ap, fmt);
    n = scan_fmt(&src, fmt);
    va_end(g_ap);
    return n;
}
