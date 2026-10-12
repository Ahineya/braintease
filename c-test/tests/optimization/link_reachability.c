#include <stdio.h>

extern int deliberately_undefined(void);
/* This definition must not create a link dependency when it is unreachable. */
int unused_export(void) { return deliberately_undefined(); }
static int table[3] = { 7, 13, 19 };
static int callback(int x) { return table[x]; }
static int (*dispatch)(int) = callback;
static int *middle = &table[1];
static int unused_table[4] = { 99, 98, 97, 96 };

int main(void) {
    if (dispatch(2) == 19) putchar('Y'); else putchar('N');
    if (*middle == 13) putchar('Y'); else putchar('N');
    table[1] = 42;
    if (dispatch(1) == 42 && *middle == 42) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
