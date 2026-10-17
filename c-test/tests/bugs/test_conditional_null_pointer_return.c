#include <stdio.h>
static unsigned short value = 42;
static unsigned short *select_pointer(int missing) {
    return missing ? 0 : &value;
}
int main(void) {
    if (select_pointer(1) == 0 && select_pointer(0) == &value && *select_pointer(0) == 42)
        putchar('Y');
    else putchar('N');
    putchar('\n');
    return 0;
}
