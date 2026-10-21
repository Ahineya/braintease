#include <stdio.h>
int main(void)
{
    int cells[3], *pointer = cells;
    if (sizeof (pointer)[0] == sizeof(int) && sizeof (cells)[1] == sizeof(int)) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
