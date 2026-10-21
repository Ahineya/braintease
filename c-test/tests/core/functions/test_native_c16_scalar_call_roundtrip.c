/* Reference behavior for the portable native backend's scalar vertical slice. */
#include <stdio.h>
static unsigned long recur(unsigned long n) {
    if (!n) return 7UL;
    return recur(n-1UL)+n;
}
static double square(double n) { return n*n+1.5; }
int main(void) {
    volatile unsigned long values[2];
    unsigned int wrap=65535U;
    values[0]=3; values[1]=9;
    putchar(recur(8)==43UL ? 'Y' : 'N');
    putchar(values[0]+values[1]==12UL ? 'Y' : 'N');
    putchar((unsigned int)(wrap+1U)==0U ? 'Y' : 'N');
    putchar(square(2.5)==7.75 ? 'Y' : 'N');
    putchar('\n');
    return 0;
}
