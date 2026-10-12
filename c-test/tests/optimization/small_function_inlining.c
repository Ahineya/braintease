#include <stdio.h>

static volatile int calls;
static int identity(int x) { return x; }
static long wide(long x) { return x + 65536L; }
static int choose(int x) { if (x < 0) return -x; return x + 3; }
static int once(void) { calls++; return calls; }
static int recursive(int x) { if (x == 0) return 1; return x * recursive(x - 1); }
static int *pointer(int *p) { return p; }
static int via_pointer(int (*f)(int), int x) { return f(x); }

int main(void) {
    int n = 7;
    if (identity(-123) == -123) putchar('Y'); else putchar('N');
    if (wide(70000L) == 135536L) putchar('Y'); else putchar('N');
    if (choose(-9) == 9 && choose(9) == 12) putchar('Y'); else putchar('N');
    if (identity(once()) == 1 && calls == 1) putchar('Y'); else putchar('N');
    if (recursive(5) == 120) putchar('Y'); else putchar('N');
    if (*pointer(&n) == 7 && pointer(&n) == &n) putchar('Y'); else putchar('N');
    if (via_pointer(identity, 17) == 17) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
