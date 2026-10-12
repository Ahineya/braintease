#include <stdio.h>
volatile int sp = 9;
volatile int gp = 13;
volatile int r0 = 17;
int main(void) {
    sp += 2;
    if (sp == 11 && gp == 13 && r0 == 17) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
