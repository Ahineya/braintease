#include <ctype.h>
#include <stdio.h>

int main() {
    if (isdigit('5')) putchar('Y'); else putchar('N');
    if (!isdigit('a')) putchar('Y'); else putchar('N');
    if (isalpha('A')) putchar('Y'); else putchar('N');
    if (!isalpha('1')) putchar('Y'); else putchar('N');
    if (isalnum('z')) putchar('Y'); else putchar('N');
    if (isspace(' ')) putchar('Y'); else putchar('N');
    if (isspace('\n')) putchar('Y'); else putchar('N');
    if (isblank('\t')) putchar('Y'); else putchar('N');
    if (!isblank('\n')) putchar('Y'); else putchar('N');
    if (isxdigit('f')) putchar('Y'); else putchar('N');
    if (isxdigit('F')) putchar('Y'); else putchar('N');
    if (!isxdigit('g')) putchar('Y'); else putchar('N');
    if (tolower('A') == 'a') putchar('Y'); else putchar('N');
    if (toupper('a') == 'A') putchar('Y'); else putchar('N');
    if (tolower('5') == '5') putchar('Y'); else putchar('N');
    if (ispunct('.')) putchar('Y'); else putchar('N');
    if (!ispunct('A')) putchar('Y'); else putchar('N');

    putchar('\n');
    return 0;
}
