#include <stdio.h>
enum { BASE = 2, COUNT = BASE * 3, NEXT };
int values[COUNT] = {1, 2, 3, 4, 5, 6};
struct Rows { int cells[NEXT]; };
int main(void)
{
    int local[COUNT + 1];
    int matrix[BASE][COUNT];
    int (*row)[COUNT] = matrix;
    putchar(sizeof(values) / sizeof(values[0]) == 6 ? 'Y' : 'N');
    putchar(sizeof(local) / sizeof(local[0]) == 7 ? 'Y' : 'N');
    putchar(sizeof(struct Rows) == 7 ? 'Y' : 'N');
    row[1][5] = 42;
    putchar(matrix[1][5] == 42 ? 'Y' : 'N');
    { enum { COUNT = 3 }; int small[COUNT]; putchar(sizeof(small) == 3 ? 'Y' : 'N'); }
    { int restored[COUNT]; putchar(sizeof(restored) == 6 ? 'Y' : 'N'); }
    putchar('\n');
    return 0;
}
