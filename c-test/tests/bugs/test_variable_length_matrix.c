#include <stdio.h>
static int matrix(int rows, int cols) {
    int values[rows][cols];
    int r, c;
    for (r = 0; r < rows; r++)
        for (c = 0; c < cols; c++) values[r][c] = r * 10 + c;
    return values[rows - 1][cols - 1];
}
int main(void) {
    if (matrix(3, 4) == 23) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
