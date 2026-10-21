#include <stdio.h>

static long twice(long value) { return value * 2; }

/* A function parameter declarator adjusts to a pointer, like an array does. */
static long apply(long callback(long), long value)
{
    return callback(value);
}

int main(void)
{
    putchar(apply(twice, 21) == 42 ? 'Y' : 'N');
    return 0;
}
