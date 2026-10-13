#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <limits.h>
#include <math.h>

static int format_variadic(char *buffer, size_t size, const char *format, ...) {
    va_list args;
    int result;
    va_start(args, format);
    result = vsnprintf(buffer, size, format, args);
    va_end(args);
    return result;
}

static int check(int condition, int test) {
    if (!condition) printf("FAIL libc test %d\n", test);
    return condition;
}

int main(void) {
    char buffer[128];
    struct {
        char before;
        char text[6];
        char after;
    } guarded;
    char *end;
    char *copy;
    int result;
    guarded.before = 'A';
    guarded.after = 'Z';
    result = snprintf(guarded.text, sizeof(guarded.text), "abcdef:%d", 42);
    if (!check(result == 9 && strcmp(guarded.text, "abcde") == 0 &&
               guarded.before == 'A' && guarded.after == 'Z', 1)) return 1;
    if (!check(snprintf(NULL, 0UL, "%s:%03d", "map", 7) == 7, 2)) return 2;
    buffer[0] = 'x';
    if (!check(snprintf(buffer, 1UL, "abc") == 3 && buffer[0] == 0, 3)) return 3;
    result = format_variadic(buffer, sizeof(buffer), "%-6.3s|%+06d|%#06x|%#.0o", "abcdef", -42, 42U, 0U);
    if (!check(result == 22 && strcmp(buffer, "abc   |-00042|0x002a|0") == 0, 4)) return 4;
    result = snprintf(buffer, sizeof(buffer), "%ld/%lu/%08lX", -2147483647L - 1L, 4294967295UL, 0x80010002UL);
    if (!check(result == 31 && strcmp(buffer, "-2147483648/4294967295/80010002") == 0, 5)) return 5;
    result = sprintf(buffer, "STTNUM%d/%c/%%/%.*s", 7, 'Q', 3, "door");
    if (!check(result == 15 && strcmp(buffer, "STTNUM7/Q/%/doo") == 0, 6)) return 6;
    result = snprintf(buffer, sizeof(buffer), "[%*d][%.0u][%.4u]", -5, 7, 0U, 12U);
    if (!check(result == 15 && strcmp(buffer, "[7    ][][0012]") == 0, 7)) return 7;
    result = snprintf(buffer, sizeof(buffer), "%zu %hd", (size_t)64000UL, (int)-123);
    if (!check(result == 10 && strcmp(buffer, "64000 -123") == 0, 8)) return 8;
    if (!check(strcasecmp("TiTlEpIc", "TITLEPIC") == 0 &&
               strncasecmp("patch1", "PATCH2", 5UL) == 0 && strcasecmp("a", "B") < 0, 9)) return 9;
    copy = strdup("E1M1/TITLEPIC");
    if (!check(copy && strcmp(copy, "E1M1/TITLEPIC") == 0 &&
               strstr(copy, "TITLEPIC") == copy + 5 && strstr(copy, "NOPE") == NULL, 10)) return 10;
    free(copy);
    if (!check(strtol(" -2147483648!", &end, 10) == (-2147483647L - 1L) && *end == '!', 11)) return 11;
    if (!check(strtoul("0xffffffff rest", &end, 0) == 4294967295UL && *end == ' ', 12)) return 12;
    if (!check(strtol("077", &end, 0) == 63L && *end == 0 && atoi(" -42") == -42, 13)) return 13;
    end = NULL;
    if (!check(strtol("no digits", &end, 10) == 0 && strcmp(end, "no digits") == 0, 14)) return 14;
#ifdef RIPPLE
    errno = 0;
    if (!check(strtol("2147483648", &end, 10) == LONG_MAX && errno == ERANGE && *end == 0, 15)) return 15;
    errno = 0;
    if (!check(strtoul("4294967296", &end, 10) == ULONG_MAX && errno == ERANGE && *end == 0, 16)) return 16;
    if (!check(fopen("unsupported.sav", "wb") == NULL && errno == ENOENT &&
               fread(buffer, 1UL, 1UL, NULL) == 0 && fwrite(buffer, 1UL, 1UL, NULL) == 0 &&
               fseek(NULL, 0L, SEEK_SET) == -1 && ftell(NULL) == -1L && fgetc(NULL) == EOF &&
               remove("unsupported.sav") == -1 && rename("a", "b") == -1, 17)) return 17;
    if (!check(snprintf(buffer, sizeof(buffer), "%n", &result) == -1 && errno == EINVAL, 18)) return 18;
    if (!check(snprintf(buffer, sizeof(buffer), "%32768d", 1) == -1 && errno == EOVERFLOW, 19)) return 19;
    if (!check(snprintf(buffer, sizeof(buffer), "%p", (void *)0) == 3 && strcmp(buffer, "0x0") == 0, 20)) return 20;
#endif
    if (!check(fabs(-2.5) == 2.5 && fabs(2.5) == 2.5 && fabs(0.0) == 0.0, 21)) return 21;
    buffer[0] = 'x';
    if (!check(snprintf(buffer, sizeof(buffer), "") == 0 && buffer[0] == 0 &&
               snprintf(NULL, 0UL, "") == 0, 22)) return 22;
    buffer[0] = 'x';
    if (!check(snprintf(buffer, 0UL, "literal run") == 11 && buffer[0] == 'x', 23)) return 23;
    result = snprintf(buffer, sizeof(buffer), "left:%s:%d:%%:right", "ab", 7);
    if (!check(result == 17 && strcmp(buffer, "left:ab:7:%:right") == 0, 24)) return 24;
    result = snprintf(guarded.text, sizeof(guarded.text), "abc%sTAIL", "de");
    if (!check(result == 9 && strcmp(guarded.text, "abcde") == 0 &&
               guarded.before == 'A' && guarded.after == 'Z', 25)) return 25;
    if (!check(snprintf(NULL, 0UL, "before:%8.3s:%-5d:after", "abcdef", 7) == 27, 26)) return 26;
    result = snprintf(guarded.text, sizeof(guarded.text), "%8s-tail", "x");
    if (!check(result == 13 && strcmp(guarded.text, "     ") == 0 &&
               guarded.before == 'A' && guarded.after == 'Z', 27)) return 27;
    if (!check(snprintf(buffer, sizeof(buffer), "%cX", 0) == 2 &&
               buffer[0] == 0 && buffer[1] == 'X' && buffer[2] == 0, 28)) return 28;
#ifdef RIPPLE
    errno = 0;
    if (!check(snprintf(buffer, sizeof(buffer), "prefix%") == -1 && errno == EINVAL &&
               strcmp(buffer, "prefix") == 0, 29)) return 29;
    result = 123;
    if (!check(snprintf(guarded.text, sizeof(guarded.text), "abc%sTAIL%n", "de", &result) == -1 &&
               errno == EINVAL && result == 123 && strcmp(guarded.text, "abcde") == 0 &&
               guarded.before == 'A' && guarded.after == 'Z', 30)) return 30;
    {
        char text[4] = {'A', (char)0x1234, 'B', 0};
        if (!check(snprintf(buffer, sizeof(buffer), text) == 3 && buffer[0] == 'A' &&
                   buffer[1] == (char)0x1234 && buffer[2] == 'B' && buffer[3] == 0, 31)) return 31;
    }
    if (!check(snprintf(NULL, 0UL, "%32767s", "") == INT_MAX, 32)) return 32;
    errno = 0;
    if (!check(snprintf(NULL, 0UL, "%32767sX", "") == -1 && errno == EOVERFLOW, 33)) return 33;
    errno = 0;
    if (!check(snprintf(NULL, 0UL, "%32767s%32767sXX", "", "") == -1 &&
               errno == EOVERFLOW, 34)) return 34;
#endif
    if (fprintf(stdout, "LIBC %-14s%d\n", "validation_ok", 1) != 21) return 35;
    return 0;
}
