#include <stdio.h>

/* C99 6.7.5.2: static specifies a minimum element count at the call site;
 * the parameter still adjusts to a pointer. Qualifiers may precede static.
 */
static int sum(const int values[static 3]) {
    return values[0] + values[1] + values[2];
}
static int last(unsigned count, const int values[const static 3]) {
    return values[count - 1];
}
static int first(const int values[static restrict 3]) {
    return values[0];
}

int main(void) {
    int values[3] = { 10, 20, 12 };
    if (sum(values) == 42) putchar('Y'); else putchar('N');
    if (last(3, values) == 12) putchar('Y'); else putchar('N');
    if (first(values) == 10) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
