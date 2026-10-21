#include <stdio.h>
#include <stdlib.h>

/* Reaching exit never falls through or returns an indeterminate value. */
static int finish(void)
{
    exit(0);
}

int main(void)
{
    putchar('Y');
    finish();
    return 1;
}
