#include <stdio.h>
static int choose(int n) {
    switch (n) {
        case 1: return 20; break;
        case 2: return 22; break;
        default: break;
    }
    return 0;
}
int main(void) {
    if (choose(1) == 20) putchar('Y'); else putchar('N');
    if (choose(2) == 22) putchar('Y'); else putchar('N');
    if (choose(3) == 0) putchar('Y'); else putchar('N');
    putchar('\n');return 0;
}
