#include <stdio.h>

static const unsigned char *table[] = {
    (const unsigned char[]){1, 3, 5},
    (const unsigned char[]){7, 9}
};

int main(void)
{
    putchar(table[0][2] == 5 && table[1][1] == 9 ? 'Y' : 'N');
    return 0;
}
