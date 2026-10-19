#include <stdint.h>
#include <stdio.h>

volatile uintptr_t input;
volatile unsigned step;
struct pair { long a, b; };
int main(void) {
    char *p, *q;
    struct pair *pairs;
    input = (9UL << 16) | 63999UL;
    p = (char *)input;
    q = p + 1;
    if ((uintptr_t)q == (10UL << 16) && (uintptr_t)p == input) putchar('Y'); else putchar('N');
    if ((uintptr_t)(q - 1) == input) putchar('Y'); else putchar('N');
    if ((uintptr_t)(p + 64002L) == ((11UL << 16) | 1UL)) putchar('Y'); else putchar('N');
    input = 9UL << 16;
    p = (char *)input;
    if ((uintptr_t)(p - 1) == ((8UL << 16) | 63999UL) && (uintptr_t)p == input) putchar('Y'); else putchar('N');
    if ((uintptr_t)(p - 64001L) == ((7UL << 16) | 63999UL)) putchar('Y'); else putchar('N');
    input = (9UL << 16) | 63998UL;
    pairs = (struct pair *)input;
    if ((uintptr_t)(pairs + 1) == ((10UL << 16) | 2UL)) putchar('Y'); else putchar('N');
    if ((uintptr_t)(pairs - 1) == ((9UL << 16) | 63994UL)) putchar('Y'); else putchar('N');
    input = (9UL << 16) | 63999UL;
    p = (char *)input;
    step = 65535U;
    if ((uintptr_t)(p + step) == ((11UL << 16) | 1534UL) && (uintptr_t)p == input) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
