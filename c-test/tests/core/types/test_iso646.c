#include <iso646.h>

void putchar(int c);

int main() {
    int a;
    int b;

    a = 1;
    b = 2;
    if (a and b) putchar('Y'); else putchar('N');
    if (a or 0) putchar('Y'); else putchar('N');
    if (not 0) putchar('Y'); else putchar('N');
    if (a not_eq b) putchar('Y'); else putchar('N');

    a = 1;
    a and_eq 3;
    if (a == 1) putchar('Y'); else putchar('N');

    a or_eq 4;
    if (a == 5) putchar('Y'); else putchar('N');

    putchar('\n');
    return 0;
}
