#include <stdio.h>
#include <stdlib.h>

static unsigned long sizes[] = {
    1, 2, 31, 32, 33, 63, 64, 65, 127, 128, 129, 255, 256, 257,
    511, 512, 513, 1023, 1024, 1025, 32767, 32768, 32769,
    63999, 64000, 64001, 65535, 65536, 65537, 131071, 131072, 131073
};
static unsigned char *blocks[32];

int main(void) {
    unsigned char *a, *b, *c, *p, *g1, *g2, *g3;
    unsigned int i, round;
    /* Several free blocks in one non-exact bin: skip undersized head and
     * interior nodes, unlink a tail/interior node without losing the others.
     */
    a = malloc(33); g1 = malloc(1);
    b = malloc(40); g2 = malloc(1);
    c = malloc(48); g3 = malloc(1);
    if (!a || !b || !c || !g1 || !g2 || !g3) return 1;
    free(c); free(b); free(a);
    p = malloc(45); if (p != c) return 2;
    b = malloc(38); if (!b) return 3;
    a = malloc(33); if (!a) return 4;
    free(a); free(b); free(p); free(g1); free(g2); free(g3);
    /* Revisit every boundary with other sizes still live, including both
     * sides of the 16-bit size boundary and multi-bank requests.
     */
    for (round = 0; round < 3; round++) {
        for (i = 0; i < 32; i++) {
            blocks[i] = malloc(sizes[i]);
            if (!blocks[i]) return 5;
            blocks[i][0] = 1000 + i;
            blocks[i][sizes[i] - 1] = 2000 + i;
        }
        for (i = 0; i < 32; i++) {
            if (blocks[i][sizes[i] - 1] != 2000 + i) return 6;
            if (sizes[i] > 1 && blocks[i][0] != 1000 + i) return 7;
        }
        for (i = 0; i < 32; i += 2) free(blocks[i]);
        for (i = 1; i < 32; i += 2) free(blocks[i]);
    }
    putchar('Y'); putchar('\n');
    return 0;
}
