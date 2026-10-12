#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    unsigned char *a = (unsigned char *)malloc(10);
    unsigned char *b = (unsigned char *)malloc(20);
    unsigned char *c = (unsigned char *)malloc(30);
    unsigned char *d;
    unsigned char *large;
    unsigned char *moved;
    unsigned char *guard;
    int i;

    if (a != NULL && b != NULL && c != NULL) putchar('Y'); else putchar('N');
    a[0] = 50000U;
    a[9] = 60000U;
    free(b);
    d = (unsigned char *)malloc(12);
    if (d == b) putchar('Y'); else putchar('N');
    free(d);
    d = (unsigned char *)realloc(a, 25);
    if (d == a && d[0] == 50000U && d[9] == 60000U) putchar('Y'); else putchar('N');
    d = (unsigned char *)realloc(d, 3);
    if (d == a && d[0] == 50000U) putchar('Y'); else putchar('N');
    free(c);
    free(d);

    large = (unsigned char *)malloc(140000UL);
    if (large != NULL) {
        large[0] = 1234;
        large[63999UL] = 2345;
        large[65536UL] = 3456;
        large[139999UL] = 4567;
        if (large[0] == 1234 && large[63999UL] == 2345 &&
            large[65536UL] == 3456 && large[139999UL] == 4567) putchar('Y'); else putchar('N');
    } else {
        putchar('N');
        return 1;
    }
    guard = (unsigned char *)malloc(16);
    guard[0] = 5678;
    moved = (unsigned char *)realloc(large, 150000UL);
    if (moved != NULL && moved != large && moved[0] == 1234 &&
        moved[65536UL] == 3456 && moved[139999UL] == 4567 &&
        guard[0] == 5678) putchar('Y'); else putchar('N');
    if (realloc(moved, SIZE_MAX) == NULL && moved[0] == 1234) putchar('Y'); else putchar('N');
    free(moved);
    free(guard);
    a = (unsigned char *)calloc(8, sizeof(unsigned char));
    for (i = 0; i < 8 && a[i] == 0; i++) { }
    if (i == 8 && calloc(SIZE_MAX, 2) == NULL && malloc(SIZE_MAX) == NULL) putchar('Y'); else putchar('N');
    if (realloc(a, 0) == NULL) putchar('Y'); else putchar('N');
    free(NULL);
    putchar('\n');
    return 0;
}
