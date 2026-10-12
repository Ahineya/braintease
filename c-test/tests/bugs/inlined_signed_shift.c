#include <stdio.h>

static int shifted(int value, unsigned int count, int expected) {
    return (value >> count) == expected;
}

int main(void) {
    for (unsigned int count = 0; count < 16; ++count) {
        if (shifted(-1, count, -1)) putchar('Y'); else putchar('N');
        if (shifted(0, count, 0)) putchar('Y'); else putchar('N');
        if (shifted(-32767 - 1, count, -(32768U >> count))) putchar('Y'); else putchar('N');
    }
    putchar('\n');
    return 0;
}
