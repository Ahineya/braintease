#include <stdarg.h>
#include <stdio.h>
static int collect(int count, const char *label, ...) {
    va_list ap;
    int n = 0;
    va_start(ap, label);
    for (int i = 0; i < count; i++) {
        const char *p = va_arg(ap, const char *);
        n += *p - '0';
    }
    va_end(ap);
    return n + *label - '0';
}
int main(void) {
    int value = collect(6, "1", "2", "3", "4", "5", "6", "7");
    puts(value == 28 ? "OK" : "FAILED");
    return value != 28;
}
