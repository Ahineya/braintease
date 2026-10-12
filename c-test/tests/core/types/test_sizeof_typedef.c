#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

typedef int myint;
typedef unsigned int u32;
typedef myint myint2;

int main() {
    if (sizeof(size_t) == 2) putchar('Y'); else putchar('N');
    if (sizeof(myint) == 1) putchar('Y'); else putchar('N');
    if (sizeof(u32) == 1) putchar('Y'); else putchar('N');
    if (sizeof(myint2) == 1) putchar('Y'); else putchar('N');
    if (sizeof(int32_t) == 2) putchar('Y'); else putchar('N');
    if (sizeof(size_t *) == 2) putchar('Y'); else putchar('N');

    putchar('\n');
    return 0;
}
