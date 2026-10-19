#include <stdio.h>
#include "../../../ports/fuzix/upstream/Library/include/math.h"
static float quiet_single = NAN;
static double quiet_double = NAN;
int main(void) {
    volatile float a = NAN;
    volatile double b = NAN;
    if(a != a) putchar('Y'); else putchar('N');
    if(b != b) putchar('Y'); else putchar('N');
    if(quiet_single != quiet_single) putchar('Y'); else putchar('N');
    if(quiet_double != quiet_double) putchar('Y'); else putchar('N');
    putchar('\n'); return 0;
}
