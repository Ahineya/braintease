#include <stdlib.h>
#include <stdio.h>

int main() {
    if (abs(-3) == 3) putchar('Y'); else putchar('N');
    if (abs(3) == 3) putchar('Y'); else putchar('N');
    if (abs(0) == 0) putchar('Y'); else putchar('N');

    if (labs(-65536L) == 65536L) putchar('Y'); else putchar('N');
    if (labs(65536L) == 65536L) putchar('Y'); else putchar('N');
    if (labs(0L) == 0L) putchar('Y'); else putchar('N');

    if (llabs(-4294967296LL) == 4294967296LL) putchar('Y'); else putchar('N');
    if (llabs(4294967296LL) == 4294967296LL) putchar('Y'); else putchar('N');
    if (llabs(0LL) == 0LL) putchar('Y'); else putchar('N');

    if (EXIT_SUCCESS == 0) putchar('Y'); else putchar('N');
    if (EXIT_FAILURE == 1) putchar('Y'); else putchar('N');

    putchar('\n');
    return 0;
}
