#include <stdio.h>
#include <stdlib.h>

int main() {
    char *pad;
    char *span;
    int i;

    pad = (char *)malloc(63996U);
    if (pad == 0) {
        putchar('N');
        putchar('\n');
        return 1;
    }

    span = (char *)malloc(10);
    if (span == 0) {
        putchar('N');
        putchar('\n');
        return 1;
    }

    for (i = 0; i < 10; i = i + 1) {
        span[i] = (char)(i + 1);
    }

    if (span[0] == 1 && span[4] == 5 && span[9] == 10) {
        putchar('Y');
    } else {
        putchar('N');
    }
    putchar('\n');

    span[0] = 42;
    if (span[0] == 42 && span[9] == 10) {
        putchar('Y');
    } else {
        putchar('N');
    }
    putchar('\n');

    return 0;
}
