#include <stdio.h>
#define main pointer_difference_probe
#include "../../../../rcc-portable/native/tests/pointer_difference.c"
#undef main
int main(void)
{
    if (pointer_difference_probe() == 0) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
