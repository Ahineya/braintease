#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* The bank word must use the same spill home on both sides of this join. */
static void *allocate(unsigned long size, int tag, void **owner) {
    void *memory = malloc(size);
    if (owner) *owner = memory;
    if (tag == 0) return 0;
    return memory;
}

int main(void) {
    void *owned = 0;
    unsigned int *first = allocate(67UL, 5, 0);
    unsigned int *second = allocate(67UL, 5, &owned);
    if ((uintptr_t)first >> 16) putchar('Y'); else putchar('N');
    if ((uintptr_t)second >> 16) putchar('Y'); else putchar('N');
    if (owned == second) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
