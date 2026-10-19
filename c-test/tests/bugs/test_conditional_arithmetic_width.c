#include <stdio.h>
static volatile int condition;
static volatile long score = 40000L;
static volatile unsigned int small = 60000U;
static volatile unsigned char byte = 1;
static long select_score(int c) { return c ? 0 : score; }
static long select_reverse(int c) { return c ? score : 0; }
int main(void)
{
    condition = 0;
    putchar(select_score(condition) == 40000L ? 'Y' : 'N');
    putchar(select_reverse(condition) == 0L ? 'Y' : 'N');
    putchar((condition ? small : score) == 40000L ? 'Y' : 'N');
    condition = 1;
    putchar(select_score(condition) == 0L ? 'Y' : 'N');
    putchar(select_reverse(condition) == 40000L ? 'Y' : 'N');
    putchar((condition ? small : score) == 60000L ? 'Y' : 'N');
    /* Equal small operand types still undergo integer promotion. */
    putchar(sizeof(condition ? byte : byte) == sizeof(int) ? 'Y' : 'N');
    putchar('\n');
    return 0;
}
