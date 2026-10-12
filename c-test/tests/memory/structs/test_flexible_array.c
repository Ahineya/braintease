#include <stdio.h>
#include <stdlib.h>

struct Packet {
    int n;
    char data[];
};

int main() {
    struct Packet *p;
    int i;

    if (sizeof(struct Packet) == 1) putchar('Y'); else putchar('N');

    /* Allocation and sizeof use the same 16-bit C-byte unit. */
    p = (struct Packet *)malloc(sizeof(struct Packet) + 4);
    if (p == 0) {
        putchar('N');
        putchar('\n');
        return 1;
    }

    p->n = 4;
    p->data[0] = 'A';
    p->data[1] = 'B';
    p->data[2] = 'C';
    p->data[3] = 'D';

    if (p->n == 4) putchar('Y'); else putchar('N');
    if (p->data[0] == 'A') putchar('Y'); else putchar('N');
    if (p->data[3] == 'D') putchar('Y'); else putchar('N');

    i = 0;
    if (p->data[i + 1] == 'B') putchar('Y'); else putchar('N');

    putchar('\n');
    return 0;
}
