#include <stdio.h>
static char text[] = "h\x1234";
int main(void)
{
    if (text[0] == 'h' && text[1] == 0x1234 && text[2] == 0)
        putchar('Y');
    else
        putchar('N');
    putchar('\n');
    return 0;
}
