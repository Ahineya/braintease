#include <stdio.h>

static unsigned short cells[] = {32768U, 65535U, 1U, 0U, 511U, 60000U, 0U, 17U, 42U};
static unsigned long words[] = {0UL, 0x12345678UL, 0xFFFF0000UL, 7UL, 0UL};
static unsigned long long wide[] = {0x0123456789ABCDEFULL, 0ULL, 0xFEDCBA9876543210ULL};
static struct Parts {
    unsigned short head[5];
    unsigned long *pointer;
    unsigned short tail[5];
} parts = {{1, 2, 3, 4, 5}, &words[1], {60000U, 0, 65535U, 8, 9}};

int main(void)
{
    if (cells[0] == 32768U && cells[1] == 65535U && cells[3] == 0U &&
        cells[4] == 511U && cells[5] == 60000U && cells[8] == 42U)
        putchar('Y');
    else
        putchar('N');
    if (words[0] == 0UL && words[1] == 0x12345678UL && words[2] == 0xFFFF0000UL &&
        words[3] == 7UL && words[4] == 0UL && wide[0] == 0x0123456789ABCDEFULL &&
        wide[1] == 0ULL && wide[2] == 0xFEDCBA9876543210ULL)
        putchar('Y');
    else
        putchar('N');
    if (parts.head[4] == 5U && *parts.pointer == words[1] && parts.tail[0] == 60000U &&
        parts.tail[1] == 0U && parts.tail[2] == 65535U && parts.tail[4] == 9U)
        putchar('Y');
    else
        putchar('N');
    putchar('\n');
    return 0;
}
