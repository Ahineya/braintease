#include <stdio.h>
volatile int source = 17;
static int double_word(int value) {
    int result;
    __asm__("ADD %0, %1, %1" : "=r"(result) : "r"(value));
    return result;
}
int main(void) {
    int before = source + source;
    int first = double_word(source);
    int second = double_word(before);
    if (first == 34) putchar('Y'); else putchar('N');
    if (second == 68) putchar('Y'); else putchar('N');
    if (before == 34) putchar('Y'); else putchar('N');
    putchar('\n'); return 0;
}
