#include <stdio.h>

int main() {
    int n;

    printf("hello %d\n", 42);
    printf("%s %c %x %%\n", "ok", 'Z', 255);
    n = printf("ab");
    if (n == 2) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
