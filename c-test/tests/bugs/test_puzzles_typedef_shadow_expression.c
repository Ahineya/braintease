#include <stdio.h>
typedef struct puzzle_grid { int n; } grid;
typedef struct puzzle_vector { int n; } vector;
int main(void)
{
    int grid = 1;
    double vector[2];
    grid = 3;
    vector[0] = 4.0;
    vector[1] = 5.0;
    if (grid == 3 && vector[0] + vector[1] == 9.0) putchar('Y');
    else putchar('N');
    putchar('\n');
    return 0;
}
