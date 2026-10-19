#include <stdio.h>
static int add(int a, int b) __attribute__((unused));
static int add(int a, int b) { return a + b; }
int main(void) {
    if (add(40, 2) == 42) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
