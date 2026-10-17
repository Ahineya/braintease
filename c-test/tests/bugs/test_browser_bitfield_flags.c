#include <stdio.h>
struct flags { unsigned first:1; unsigned second:1; signed small:3; };
int main(void) {
    struct flags f = {0, 1, -1};
    f.first = 1;
    if (f.first == 1 && f.second == 1 && f.small == -1) putchar('Y'); else putchar('N');
    f.first = 3;
    f.second = 0;
    if (f.first == 1 && f.second == 0 && f.small == -1) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
