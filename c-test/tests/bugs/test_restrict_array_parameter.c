#include <stdio.h>
static int total(const int values[restrict 3]) {
    return values[0] + values[1] + values[2];
}
int main(void) {
    int values[3] = { 10, 20, 12 };
    if (total(values) == 42) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
