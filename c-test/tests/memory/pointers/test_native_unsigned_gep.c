#include <stdint.h>
#include <stdio.h>

static volatile unsigned int input = 65535U;
static uintptr_t native_offset(unsigned int *base, unsigned int index)
{
    unsigned int *result = base + index;
    /* Both operands remain live after the GEP. */
    if (index != input || (uintptr_t)base != 0x0005f618UL)
        return 0;
    return (uintptr_t)result;
}

int main(void)
{
    unsigned int *base = (unsigned int *)0x0005f618UL;
    unsigned int index = input;
    volatile long wide = 65536L;
    volatile int negative = -2;
    putchar(native_offset(base, index) == 0x00070217UL ? 'Y' : 'N');
    putchar((uintptr_t)(base + index) == 0x00070217UL ? 'Y' : 'N');
    putchar((uintptr_t)(base + wide) == 0x00070218UL ? 'Y' : 'N');
    putchar((uintptr_t)(base + negative) == 0x0005f616UL ? 'Y' : 'N');
    base = (unsigned int *)0x0005ffffUL;
    putchar((uintptr_t)(base + index) == 0x00070bfeUL ? 'Y' : 'N');
    putchar(index == 65535U ? 'Y' : 'N');
    putchar('\n');
    return 0;
}
