#include <stdarg.h>

void putchar(int c);

int print_ull(unsigned long long n) {
    char buf[32];
    int i;
    unsigned long long b;

    b = 10ULL;
    i = 0;
    if (n == 0) {
        putchar('0');
        return 1;
    }
    while (n > 0) {
        buf[i] = (char)('0' + (int)(n % b));
        i = i + 1;
        n = n / b;
        if (i > 30) {
            putchar('F');
            return -1;
        }
    }
    while (i) {
        i = i - 1;
        putchar(buf[i]);
    }
    return 0;
}

void take_ull(char *fmt, ...) {
    va_list ap;
    unsigned long long n;

    va_start(ap, fmt);
    n = va_arg(ap, unsigned long long);
    print_ull(n);
}

int main() {
    print_ull(4294967296ULL);
    putchar('\n');
    take_ull("", 4294967296ULL);
    putchar('\n');
    return 0;
}
