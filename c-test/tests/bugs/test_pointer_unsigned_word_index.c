#include <stdint.h>
#include <stdio.h>
volatile unsigned index;
volatile int negative;
int main(void) {
    char *p = (char *)((9UL << 16) | 63999UL);
    index = 65535U;
    if ((uintptr_t)(p + index) == ((11UL << 16) | 1534UL)) putchar('Y'); else putchar('N');
    index = 32768U;
    if ((uintptr_t)(p + index) == ((10UL << 16) | 32767UL)) putchar('Y'); else putchar('N');
    index = 65535U;
    if ((uintptr_t)((long *)p + index) == ((12UL << 16) | 3069UL)) putchar('Y'); else putchar('N');
    negative = -32768;
    if ((uintptr_t)(p + negative) == ((9UL << 16) | 31231UL)) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
