#include <stdio.h>
#include <stdlib.h>

int main() {
    int *p;
    int i;
    int ok;

    p = (int *)calloc(4, sizeof(int));
    if (p == 0) {
        putchar('N');
        putchar('\n');
        return 1;
    }

    ok = 1;
    for (i = 0; i < 4; i = i + 1) {
        if (p[i] != 0) {
            ok = 0;
        }
    }
    if (ok) putchar('Y'); else putchar('N');
    putchar('\n');

    p[0] = 1;
    p[3] = 7;
    if (p[0] == 1 && p[1] == 0 && p[3] == 7) putchar('Y'); else putchar('N');
    putchar('\n');

    return 0;
}
