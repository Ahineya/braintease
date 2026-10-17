#include <stdio.h>
volatile unsigned long source = 100001UL;
static unsigned long combine(unsigned long a, unsigned long b, unsigned long c,
    unsigned long d, unsigned long e, unsigned long f, unsigned long g,
    unsigned long h, unsigned long i, unsigned long j) {
    __asm__("ADDI R0, R0, 0" : : : "memory");
    return a+b+c+d+e+f+g+h+i+j;
}
int main(void) {
    unsigned long result = combine(source,source*2,source*3,source*4,source*5,
        source*6,source*7,source*8,source*9,source*10);
    if (result == 5500055UL) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
