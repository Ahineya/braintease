/* C99 unsigned conversion for the compiler's guest bootstrap support library. */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>

static int digit(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z')
        return c - 'A' + 10;
    return -1;
}

unsigned long long strtoull(const char *source, char **end, int base)
{
    const char *p = source;
    unsigned long long value = 0, cutoff;
    int negative = 0, any = 0, overflow = 0, d, remainder;
    if (end)
        *end = (char *)source;
    if (base && (base < 2 || base > 36)) {
        errno = EINVAL;
        return 0;
    }
    while (isspace((unsigned char)*p))
        p++;
    if (*p == '+' || *p == '-')
        negative = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
        digit(p[2]) >= 0 && digit(p[2]) < 16) {
        base = 16;
        p += 2;
    }
    if (!base)
        base = *p == '0' ? 8 : 10;
    cutoff = ULLONG_MAX / (unsigned)base;
    remainder = ULLONG_MAX % (unsigned)base;
    while ((d = digit(*p)) >= 0 && d < base) {
        any = 1;
        if (value > cutoff || (value == cutoff && d > remainder))
            overflow = 1;
        else
            value = value * (unsigned)base + (unsigned)d;
        p++;
    }
    if (end && any)
        *end = (char *)p;
    if (overflow) {
        errno = ERANGE;
        return ULLONG_MAX;
    }
    return negative ? 0ULL - value : value;
}
