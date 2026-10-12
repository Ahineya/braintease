#include <stdio.h>

typedef struct { long x; int y; } Pair;
static Pair pairs[] = {{70000L, 2}, {-90000L, 3}, {4L, 5}};
extern int declared[];
int declared[] = {[4] = 8, 9, [1] = 6};
static char greeting[] = "Doom";
static unsigned char braced[] = {"bank"};
static unsigned long total = sizeof(pairs) / sizeof(pairs[0]);

int main(void)
{
    Pair local[] = {{1L, 2}, {3L, 4}};
    int sparse[] = {[5] = 12, 13, [2] = 9};
    char text[] = "cells";
    unsigned char brace_text[] = {"wide"};
    char exact[3] = "ABC";
    char padded[6] = "AB";
    putchar(sizeof(pairs) / sizeof(pairs[0]) == 3 && total == 3 ? 'Y' : 'N');
    putchar(pairs[1].x == -90000L && pairs[2].y == 5 ? 'Y' : 'N');
    putchar(sizeof(declared) / sizeof(declared[0]) == 6 && declared[5] == 9 ? 'Y' : 'N');
    putchar(sizeof(local) / sizeof(local[0]) == 2 && local[1].y == 4 ? 'Y' : 'N');
    putchar(sizeof(sparse) / sizeof(sparse[0]) == 7 && sparse[0] == 0 && sparse[6] == 13 ? 'Y' : 'N');
    putchar(sizeof(greeting) == 5 && sizeof(braced) == 5 && braced[4] == 0 ? 'Y' : 'N');
    putchar(sizeof(text) == 6 && sizeof(brace_text) == 5 && text[5] == 0 ? 'Y' : 'N');
    putchar(sizeof("hi") == 3 && sizeof(exact) == 3 && exact[2] == 'C' ? 'Y' : 'N');
    putchar(padded[2] == 0 && padded[5] == 0 ? 'Y' : 'N');
    putchar('\n');
    return 0;
}
