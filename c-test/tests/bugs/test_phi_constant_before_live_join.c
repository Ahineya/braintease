#include <stdio.h>
int choose(int flag, int value) {
    int result = value, ready = 0;
    if (flag) { result = 99; ready = 1; }
    else { ready = 1; }
    return result + ready;
}
int main(void) {
    if (choose(0, 12) == 13) putchar('Y'); else putchar('N');
    if (choose(1, 12) == 100) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
