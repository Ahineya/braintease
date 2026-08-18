#include <stdint.h>

void putchar(int c);

/* Prototype-only type, same as wad.h: the definition is later so the
 * call uses the declaration's parameter types. */
void clobber(int a, int b);
uint32_t take32(uint32_t x);

/* Leave a nonzero high word in A1, the way printf does before wad_seek(4). */
void clobber(int a, int b) {
    (void)a;
    (void)b;
}

int main() {
    clobber(1, 2);
    if (take32(0) == 0UL) putchar('Y'); else putchar('N');
    clobber(1, 2);
    if (take32(4) == 4UL) putchar('Y'); else putchar('N');
    clobber(1, 2);
    if (take32(8) == 8UL) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}

uint32_t take32(uint32_t x) {
    return x;
}
