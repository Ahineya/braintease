#include <stdio.h>
#include "../../../../rcc-portable/native/tests/variadic/callee.c"
#define main variadic_probe_main
#include "../../../../rcc-portable/native/tests/variadic/main.c"
#undef main
int main(void)
{
    if (variadic_probe_main() == 0) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
