#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct record { long tag; int *value; };
static int answer = 42;
static struct record records[2] = {{123456L, &answer}, {654321L, &answer}};
static unsigned cells[64010];
static int add(int x) { return x + 10; }
static int sub(int x) { return x - 10; }
static int (*callbacks[2])(int) = {add, sub};

int main(void) {
    unsigned *p, *q;
    char text[12];
    volatile float x = 16777216.0f, one = 1.0f;
    double values[2] = {1.25, 2.5};
    if (records[1].tag == 654321L && *records[1].value == 42) putchar('Y'); else putchar('N');
    cells[64005L] = 1234;
    if (cells[64005L] == 1234 && !cells[64009L]) putchar('Y'); else putchar('N');
    if (callbacks[0](5) == 15 && callbacks[1](25) == 15) putchar('Y'); else putchar('N');
    p = malloc(64020UL);
    if (!p) return 1;
    p[0] = 123; p[64019L] = 456;
    q = realloc(p, 65000UL);
    if (q && q[0] == 123 && q[64019L] == 456) putchar('Y'); else putchar('N');
    free(q);
    p = calloc(64020UL, 1UL);
    if (p && !p[0] && !p[64019L]) putchar('Y'); else putchar('N');
    free(p);
    strcpy(text, "Ripple");
    if (strlen(text) == 6UL && strcmp(text, "Ripple") == 0) putchar('Y'); else putchar('N');
    if (x + one == x && values[0] * 2.0 + values[1] == 5.0) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
