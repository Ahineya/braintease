#include <stdio.h>
#include <stdlib.h>

union pointer_bits { void *pointer; unsigned int words[2]; };

int main(void) {
    unsigned int cut, i;
    unsigned char *pad, *p, *guard, *grown;
    union pointer_bits bits;
    /* Exercise every split of an eleven-cell header across two banks.
     * This includes the 32-bit size and each physical/free-list pointer.
     */
    for (cut = 1; cut < 11; cut++) {
        pad = malloc(64000UL - 11 - cut);
        p = malloc(33); guard = malloc(20);
        if (!pad || !p || !guard) return 1;
        bits.pointer = p;
        if (bits.words[0] != 11 - cut || bits.words[1] != 6) return 2;
        for (i = 0; i < 33; i++) p[i] = 500 + i;
        guard[0] = 900;
        /* Force copying, then coalesce the old cross-bank header backwards. */
        grown = realloc(p, 129);
        if (!grown) return 3;
        for (i = 0; i < 33; i++) if (grown[i] != 500 + i) return 4;
        free(pad); free(guard);
        if (grown[32] != 532) return 5;
        free(grown);
    }
    putchar('Y'); putchar('\n');
    return 0;
}
