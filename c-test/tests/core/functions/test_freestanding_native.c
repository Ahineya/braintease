#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int global = 17;
static volatile double factor = 1.5;
static int add(int a, int b) { return a + b + global; }
static int (*callback)(int, int) = add;
static void check(int condition) { putchar(condition ? 'Y' : 'N'); }

int main(void) {
    check(sizeof(int) == 1 && sizeof(void *) == 2);
    check(callback(10, 15) == 42);
    unsigned long long wide = 0xfedcba9876543210ULL;
    check((wide >> 32) == 0xfedcba98UL);
    unsigned char *data = calloc(128, sizeof(*data));
    check(data != 0 && data[127] == 0);
    if (!data) { putchar('\n'); return 1; }
    memset(data, 65535U, 128);
    check(data[0] == 65535U && data[127] == 65535U);
    memcpy(data, "freestanding", 13);
    check(strcmp(data, "freestanding") == 0);
    memmove(data + 1, data, 13);
    check(strcmp(data + 1, "freestanding") == 0);
    free(data);
    check(factor * 2.0 == 3.0);
    putchar('\n');
    return 0;
}
