/* Bounded HTTP grammar/number helpers shared by parsing and serialization. */
#include "http_internal.h"
int http_token(unsigned short c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        (c && strchr("!#$%&'*+-.^_`|~", c) != 0);
}
int http_equal(const char *a, const char *b) {
    unsigned short c;
    while (*a && *b) {
        c = *a++; if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != *b++) return 0;
    }
    return !*a && !*b;
}
int http_decimal(const char *text, unsigned long *value) {
    unsigned short digit;
    unsigned long result = 0;
    if (!*text) return 0;
    while (*text) {
        if (*text < '0' || *text > '9') return 0;
        digit = *text++ - '0';
        if (result > 429496729UL || (result == 429496729UL && digit > 5)) return 0;
        result = result * 10UL + digit;
    }
    *value = result; return 1;
}
/* Caller provides at least 10 cells. Returns count, without a trailing NUL. */
unsigned short http_format_decimal(char *output, unsigned long value) {
    char digits[10];
    unsigned short used = 0, i;
    do { digits[used++] = '0' + value % 10UL; value /= 10UL; } while (value);
    for (i = 0; i < used; ++i) output[i] = digits[used - 1 - i];
    return used;
}
