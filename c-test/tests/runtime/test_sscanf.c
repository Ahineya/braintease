#include <stdio.h>

int main() {
    char c;
    int n;
    int d;
    int e;
    int u;
    char word[16];
    long lv;

    n = sscanf("A", "%c", &c);
    if (n == 1 && c == 'A') putchar('Y'); else putchar('N');

    n = sscanf("Z", "%c", &c);
    if (n == 1 && c == 'Z' && (int)c == 90) putchar('Y'); else putchar('N');

    n = sscanf("  -42x", "%d", &d);
    if (n == 1 && d == -42) putchar('Y'); else putchar('N');

    n = sscanf("2A", "%x", &u);
    if (n == 1 && u == 42) putchar('Y'); else putchar('N');

    n = sscanf("  hello world", "%s", word);
    if (n == 1 && word[0] == 'h' && word[4] == 'o' && word[5] == 0) putchar('Y'); else putchar('N');

    n = sscanf("9 8", "%d %d", &d, &e);
    if (n == 2 && d == 9 && e == 8) putchar('Y'); else putchar('N');

    n = sscanf("100", "%ld", &lv);
    if (n == 1 && lv == 100L) putchar('Y'); else putchar('N');

    n = sscanf("ab", "a%c", &c);
    if (n == 1 && c == 'b') putchar('Y'); else putchar('N');

    n = sscanf("0x10", "%i", &d);
    if (n == 1 && d == 16) putchar('Y'); else putchar('N');

    n = sscanf("", "%c", &c);
    if (n == EOF) putchar('Y'); else putchar('N');

    putchar('\n');
    return 0;
}
