#include <stdio.h>
int main(void) {
    const unsigned char bytes[] = "\xd0\x96";
    if (sizeof(bytes) == 3) putchar('Y'); else putchar('N');
    if (bytes[0] == 0xd0 && bytes[1] == 0x96 && bytes[2] == 0) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
