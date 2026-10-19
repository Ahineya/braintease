#include <stdio.h>
typedef struct bookmark { int value; } bookmark;
static int read_value(bookmark *bookmark) { return bookmark->value; }
int main(void) {
    bookmark bookmark;
    bookmark.value = 42;
    if (read_value(&bookmark) == 42) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
