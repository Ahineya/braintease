/* Compiler-local hexadecimal conversion; decimal conversion stays in libc. */
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#undef strtod

static int hex_digit(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

double rcc_c_strtod(const char *text, char **end)
{
    const char *p = text, *exponent;
    double value = 0.0;
    int negative, digit, digits = 0, fraction = 0, kept = 0, sign = 1, nonzero;
    long power = 0;
    while (isspace((unsigned char)*p))
        p++;
    negative = *p == '-';
    if (*p == '-' || *p == '+')
        p++;
    if (p[0] != '0' || (p[1] != 'x' && p[1] != 'X'))
        return strtod(text, end);
    p += 2;
    while ((digit = hex_digit(*p)) >= 0 || (*p == '.' && !fraction)) {
        if (*p == '.')
            fraction = 1;
        else {
            digits = 1;
            if (kept < 14) {
                value = value * 16.0 + digit;
                if (value != 0.0 || kept)
                    kept++;
                if (fraction)
                    power -= 4;
            } else if (!fraction)
                power += 4;
        }
        p++;
    }
    if (!digits)
        return strtod(text, end);
    if (*p == 'p' || *p == 'P') {
        long magnitude = 0;
        exponent = p++;
        if (*p == '-' || *p == '+') {
            if (*p == '-')
                sign = -1;
            p++;
        }
        if (!isdigit((unsigned char)*p))
            p = exponent;
        else {
            while (isdigit((unsigned char)*p)) {
                if (magnitude < 4096)
                    magnitude = magnitude * 10 + *p - '0';
                p++;
            }
            power += sign * magnitude;
        }
    }
    nonzero = value != 0.0;
    while (power > 0 && value != value * 0.5) {
        value *= 2.0;
        power--;
    }
    while (power < 0 && value != 0.0) {
        value *= 0.5;
        power++;
    }
    if (nonzero && (value == 0.0 || value == value * 0.5))
        errno = ERANGE;
    if (end)
        *end = (char *)p;
    return negative ? -value : value;
}
