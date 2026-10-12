#include <stdio.h>

/* Declared assembly clobbers must preserve unrelated live C values. */
int clobber_leaf(int value) {
    __asm__("LI S3, 17" : : : "s3");
    return value;
}

int main(void) {
    if (clobber_leaf(1234) == 1234) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
