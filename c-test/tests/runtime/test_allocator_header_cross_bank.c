#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    unsigned int n;
    unsigned char *pad;
    unsigned char *p;
    unsigned char *q;
    int ok = 1;
    /* The next header starts in each of the final four cells of bank 5.
     * Its size and next-pointer words must be read/stored across banks. */
    for (n = 63991U; n <= 63994U; n++) {
        pad = (unsigned char *)malloc(n);
        p = (unsigned char *)malloc(8);
        q = (unsigned char *)malloc(16);
        if (pad == NULL || p == NULL || q == NULL) return 1;
        p[0] = 50000U;
        p[7] = 60000U;
        q[0] = 1234;
        free(q);
        q = (unsigned char *)realloc(p, 32);
        if (q != p || q[0] != 50000U || q[7] != 60000U) ok = 0;
        free(q);
        free(pad);
    }
    putchar(ok ? 'Y' : 'N');
    putchar('\n');
    return 0;
}
