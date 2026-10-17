#include <stdio.h>
typedef int (row)[3];
int main(void) {
    row values = {4, 7, 9};
    if (sizeof(values) == 3 && values[1] == 7) putchar('Y'); else putchar('N');
    putchar('\n'); return 0;
}
