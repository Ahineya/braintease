#include <stdio.h>

static int value;

int main(void) {
    int *volatile pointer = &value;
    _Bool result = 0;
    result = pointer;
    if (result == 1) putchar('Y'); else putchar('N');
    pointer = 0;
    result = pointer;
    if (result == 0) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
