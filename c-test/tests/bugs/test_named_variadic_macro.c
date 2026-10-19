#include <stdio.h>
#define SUM(first, rest...) ((first) + (rest))
int main(void) {
    if (SUM(40, 2) == 42) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
