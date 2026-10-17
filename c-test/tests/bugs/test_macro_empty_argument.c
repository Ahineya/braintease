#include <stdio.h>
#define VALUE(ignored) 7
#define ATTR_TYPE(ignored)
static int values[] = {1 ATTR_TYPE(), 2 ATTR_TYPE()};
int main(void) {
    if (VALUE() == 7 && values[0] == 1 && values[1] == 2) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
