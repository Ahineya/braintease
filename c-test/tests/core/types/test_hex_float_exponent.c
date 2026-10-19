#include <stdio.h>

/* QuickJS cutils.h uses hexadecimal floating literals, valid since C99. */
int main(void)
{
    double small = 0x1.8p+1;
    double huge = 0x1p1008;
    if (small == 3.0) putchar('Y'); else putchar('N');
    if (huge > 1e300 && huge < 1e305) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
