#include <stdio.h>
static int value(void)
{
    return 7;
    { int unused[1]; unused[0] = 0; }
}
int main(void)
{
    if (value() == 7) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
