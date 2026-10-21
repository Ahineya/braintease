#include <stdio.h>
struct result { unsigned long value; unsigned long next; int *p; };
static struct result make(unsigned long n, int *p)
{
    struct result r = {n, n + 1, p};
    return r;
}
static unsigned long combine(struct result a, struct result b)
{
    a.value += 2;
    return a.value + b.next;
}
int main(void)
{
    int value = 7;
    struct result a = make(10, &value);
    if (a.value == 10 && a.next == 11 && a.p == &value) putchar('Y'); else putchar('N');
    if (combine(make(20, &value), make(30, &value)) == 53) putchar('Y'); else putchar('N');
    if (a.value == 10 && *a.p == 7) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
