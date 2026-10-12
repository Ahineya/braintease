#include <limits.h>
void putchar(int c) { __asm__("STORE A0, R0, R0"); }
int main(void) {
    unsigned char a = (unsigned char)4660L;
    if (sizeof(int) == 1 && CHAR_BIT == 16 && a == 4660U) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
