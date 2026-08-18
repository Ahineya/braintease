// User BSS must not overlap libruntime statics after linking.
// Filling a large global and then using malloc fails if both objects
// still start at GP address 0.
#include <stdio.h>
#include <stdlib.h>

char pad[200];
int marker = 65;

int main() {
    int i;
    int *p;

    for (i = 0; i < 200; i++) {
        pad[i] = 1;
    }

    p = (int *)malloc(sizeof(int));
    if (p == 0) {
        putchar('N');
        putchar('\n');
        return 1;
    }
    *p = 42;
    if (*p == 42 && marker == 65 && pad[0] == 1 && pad[199] == 1) {
        putchar('Y');
    } else {
        putchar('N');
    }
    putchar('\n');
    return 0;
}
