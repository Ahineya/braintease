#include <stdio.h>
#define BITFIELD_GLOBALS 1
#define main bitfield_probe
#include "../../../../rcc-portable/native/tests/bitfields.c"
#undef main
int main(void)
{
    if (bitfield_probe() == 0) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
