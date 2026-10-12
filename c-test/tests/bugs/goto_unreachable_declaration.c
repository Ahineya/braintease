#include <stdio.h>

int main(void) {
    goto assigned;
    int x = 99;
assigned:
    x = 7;
    if (x == 7) putchar('Y'); else putchar('N');
    goto second;
    x = 42;
second:
    if (x == 7) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
