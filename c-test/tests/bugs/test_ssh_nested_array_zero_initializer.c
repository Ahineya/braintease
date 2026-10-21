#include <stdio.h>
struct packet { unsigned char data[8]; unsigned size; };
int main(void) {
    struct packet p = {0};
    unsigned i;
    if (p.size == 0) putchar('Y'); else putchar('N');
    for (i = 0; i < 8; i++) {
        if (p.data[i] == 0) putchar('Y'); else putchar('N');
    }
    putchar('\n');
    return 0;
}
