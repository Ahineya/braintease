/* Guest adapters for NetHack 3.6.7. See upstream/dat/license. */
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdlib.h>
#include <limits.h>

/* NetHack's two non-DLB fscanf users read integer fields only. Keep the
 * stream positioned immediately after the matched fields, including widths. */
int nh_fscanf(FILE *file, const char *format, ...)
{
    va_list args;
    int assignments = 0, consumed = 0, c = 0;
    va_start(args, format);
    while (*format) {
        unsigned width = UINT_MAX;
        int wide = 0, base = 10, negative = 0, digits = 0, spec;
        unsigned long value = 0;
        if (isspace(*format)) {
            while (isspace(*format)) ++format;
            do { c = fgetc(file); } while (c != EOF && isspace(c));
            if (c != EOF) ungetc(c,file);
            continue;
        }
        if (*format != '%') {
            c = fgetc(file);
            if (c != *format++) { if (c != EOF) ungetc(c,file); break; }
            consumed = 1;
            continue;
        }
        ++format;
        if (isdigit(*format)) {
            width = 0;
            while (isdigit(*format)) width = width * 10 + (*format++ - '0');
        }
        if (*format == 'l') { wide = 1; ++format; }
        spec = *format++;
        if (spec != 'd' && spec != 'u' && spec != 'x') break;
        if (spec == 'x') base = 16;
        do { c = fgetc(file); } while (c != EOF && isspace(c));
        if (width && (c == '-' || c == '+')) {
            negative = c == '-'; --width; c = fgetc(file);
        }
        while (c != EOF && width) {
            int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;
            if (digit >= base) break;
            value = value * base + digit;
            ++digits; --width;
            c = fgetc(file);
        }
        if (c != EOF) ungetc(c,file);
        if (!digits) break;
        if (negative) value = 0UL - value;
        if (wide) {
            if (spec == 'd') *va_arg(args,long *) = (long)value;
            else *va_arg(args,unsigned long *) = value;
        } else {
            if (spec == 'd') *va_arg(args,int *) = (int)value;
            else *va_arg(args,unsigned *) = (unsigned)value;
        }
        ++assignments; consumed = 1;
    }
    va_end(args);
    return !consumed && c == EOF ? EOF : assignments;
}

#include <time.h>
#include <errno.h>

/* Convert NetHack's local calendar timestamps back to the guest's unsigned
 * 32-bit Unix time. Use localtime_r for the configured timezone/DST rules. */
static long long calendar_seconds(const struct tm *t)
{
    long year = (long)t->tm_year + 1900, month = t->tm_mon;
    long y, days = 0, m;
    static const int month_days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    year += month / 12; month %= 12;
    if (month < 0) { month += 12; --year; }
    if (year < 1969 || year > 2107) return -99999999999LL;
    for (y = 1970; y < year; ++y) days += __isleap(y) ? 366 : 365;
    for (y = 1969; y >= year; --y) days -= __isleap(y) ? 366 : 365;
    for (m = 0; m < month; ++m) days += month_days[m] + (m == 1 && __isleap(year));
    days += (long)t->tm_mday - 1;
    return (long long)days * 86400 + (long)t->tm_hour * 3600L + (long)t->tm_min * 60L + t->tm_sec;
}
time_t mktime(struct tm *input)
{
    struct tm local;
    long long desired = calendar_seconds(input), guess = desired;
    time_t result;
    int pass;
    tzset(); guess += timezone;
    if (input->tm_isdst > 0) guess -= 3600;
    for (pass = 0; pass < 4; ++pass) {
        long long delta;
        if (guess < 0 || guess > 4294967295LL) break;
        result = (time_t)guess;
        if (!localtime_r(&result,&local)) break;
        delta = desired - calendar_seconds(&local);
        if (!delta) { *input = local; return result; }
        guess += delta;
    }
    errno = EOVERFLOW;
    return (time_t)-1;
}
