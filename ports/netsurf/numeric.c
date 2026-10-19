/* Port-local conversions absent from the guest libc's base profile. */
#include <netsurf_profile.h>
#include <ctype.h>
#include <float.h>
#include <math.h>

static unsigned digit_value(unsigned ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'Z') return ch - 'A' + 10;
    return 36;
}

static uint64_t parse_integer(const char *text, char **end, int base,
                             int *negative, uint64_t positive_limit, uint64_t negative_limit)
{
    const char *p = text, *digits;
    uint64_t value = 0, limit;
    int overflow = 0;
    unsigned digit;
    if (end) *end = (char *)text;
    *negative = 0;
    if (base && (base < 2 || base > 36)) { errno = EINVAL; return 0; }
    while (isspace((unsigned char)*p)) p++;
    *negative = *p == '-';
    if (*p == '+' || *p == '-') p++;
    if ((base == 0 || base == 16) && p[0] == '0' &&
        (p[1] == 'x' || p[1] == 'X') && digit_value(p[2]) < 16) {
        base = 16; p += 2;
    }
    if (!base) base = *p == '0' ? 8 : 10;
    limit = *negative ? negative_limit : positive_limit;
    digits = p;
    while ((digit = digit_value((unsigned char)*p)) < (unsigned)base) {
        if (value > (limit - digit) / (unsigned)base) overflow = 1;
        else if (!overflow) value = value * (unsigned)base + digit;
        p++;
    }
    if (p == digits) return 0;
    if (end) *end = (char *)p;
    if (overflow) { errno = ERANGE; return limit; }
    return value;
}

long long ns_strtoll(const char *text, char **end, int base)
{
    int negative;
    uint64_t maximum = UINT64_MAX >> 1;
    uint64_t value = parse_integer(text, end, base, &negative, maximum, maximum + 1);
    if (!negative) return (long long)value;
    if (value == maximum + 1) return -((long long)maximum) - 1;
    return -(long long)value;
}

unsigned long long ns_strtoull(const char *text, char **end, int base)
{
    int negative, saved = errno;
    uint64_t value;
    errno = 0;
    value = parse_integer(text, end, base, &negative, UINT64_MAX, UINT64_MAX);
    if (errno) return value;
    errno = saved;
    return negative ? (uint64_t)0 - value : value;
}

float ns_strtof(const char *text, char **end)
{
    double value = strtod(text, end);
    float result = (float)value;
    if (isfinite(value) && ((value > FLT_MAX || value < -FLT_MAX) || (value != 0.0 && result == 0.0f))) errno = ERANGE;
    return result;
}

/* Keep this private conversion compatible with both the original floor-only
 * SDK and the expanded SDK, which now supplies its own public ceil. */
float ns_ceilf(float value)
{
    return (float)-floor(-(double)value);
}
