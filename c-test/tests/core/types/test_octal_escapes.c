#include <stdio.h>

int main() {
    char *s;

    if (010 == 8) putchar('Y'); else putchar('N');
    if (077 == 63) putchar('Y'); else putchar('N');
    if (0 == 0) putchar('Y'); else putchar('N');

    if ('\a' == 7) putchar('Y'); else putchar('N');
    if ('\b' == 8) putchar('Y'); else putchar('N');
    if ('\f' == 12) putchar('Y'); else putchar('N');
    if ('\v' == 11) putchar('Y'); else putchar('N');
    if ('\x41' == 'A') putchar('Y'); else putchar('N');
    if ('\012' == 10) putchar('Y'); else putchar('N');
    if ('\0' == 0) putchar('Y'); else putchar('N');
    if ('\?' == '?') putchar('Y'); else putchar('N');

    s = "A\0B";
    if (s[0] == 'A') putchar('Y'); else putchar('N');
    if (s[1] == 0) putchar('Y'); else putchar('N');
    if (s[2] == 'B') putchar('Y'); else putchar('N');

    putchar('\n');
    return 0;
}
