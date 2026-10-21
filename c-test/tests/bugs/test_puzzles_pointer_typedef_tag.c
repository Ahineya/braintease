#include <stdio.h>
typedef struct pegrow { int count; } *pegrow;
int main(void)
{
    struct pegrow storage;
    pegrow row = (struct pegrow *)&storage;
    row->count = 7;
    if (row->count == 7 && sizeof(pegrow) == sizeof(void *)) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
