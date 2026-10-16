#include <stdio.h>
/* Numeric escapes denote individual execution characters, not UTF-8 bytes. */
int main(void) {
    const unsigned char *s = (const unsigned char *)"\300\250\100\001";
    if (s[0] == 192 && s[1] == 168 && s[2] == 64 && s[3] == 1 && s[4] == 0) putchar('Y'); else putchar('N');
    if (sizeof("\300\250\100\001") == 5) putchar('Y'); else putchar('N');
    putchar('\n'); return 0;
}
