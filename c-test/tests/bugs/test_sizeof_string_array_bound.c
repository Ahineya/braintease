/* sizeof an array initialized from a string is an integer constant expression,
 * including when it is used as another array's bound. */
#include <stdio.h>
static const char expected[] = "abc";
int main(void) {
    char buffer[sizeof(expected)];
    if (sizeof(buffer) == 4) putchar('Y'); else putchar('N');
    buffer[3] = 'X';
    if (buffer[3] == 'X') putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
