/* C99 6.3.2.3 / 6.7.8: an array of unsigned char may be accessed
 * through a char * after a conversion.  (char *)arr is valid.
 *
 * rcc: Code generation error: Unsupported construct: cast from
 * Array { UnsignedChar } to Pointer { Char }.
 */
#include <stdio.h>

int main() {
    unsigned char raw[4];
    char *p;
    int i;

    raw[0] = 'A';
    raw[1] = 'B';
    raw[2] = 'C';
    raw[3] = 'D';

    p = (char *)raw;
    if (p[0] == 'A' && p[3] == 'D') putchar('Y'); else putchar('N');

    i = 0;
    if (((char *)raw)[i] == 'A') putchar('Y'); else putchar('N');

    putchar('\n');
    return 0;
}
