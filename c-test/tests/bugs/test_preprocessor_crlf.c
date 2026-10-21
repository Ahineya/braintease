#include <stdio.h>
#define VALUE (1 + \
2)
int main(void) {
    putchar(VALUE == 3 ? 'Y' : 'N');
    putchar('\n');
    return 0;
}
