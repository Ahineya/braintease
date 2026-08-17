#include <stdio.h>

int main() {
    int a[1 + 2];
    int i;
    int b[sizeof(int) * 2];
    int d[4] = { [1 + 1] = 7 };

    for (i = 0; i < 3; i = i + 1) {
        a[i] = i + 1;
    }
    if (a[0] == 1 && a[1] == 2 && a[2] == 3) putchar('Y'); else putchar('N');

    if (sizeof(b) == 8) putchar('Y'); else putchar('N');

    if (d[2] == 7 && d[0] == 0) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
