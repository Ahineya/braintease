#include <stdio.h>
#include <stdlib.h>

#define SLOTS 32
static unsigned char *slots[SLOTS];
static unsigned int lengths[SLOTS];
static unsigned int values[SLOTS];

static int valid(unsigned int slot) {
    unsigned int i;
    for (i = 0; i < lengths[slot]; i++)
        if (slots[slot][i] != values[slot]) return 0;
    return 1;
}

int main(void) {
    unsigned long state = 12345;
    unsigned int step, slot, size, op, i;
    unsigned char *p;
    /* Keep unrelated live allocations while repeatedly changing size classes,
     * unlinking interior free nodes, splitting, and coalescing in both orders.
     */
    for (step = 0; step < 1024; step++) {
        state = state * 1664525UL + 1013904223UL;
        slot = (unsigned int)(state >> 16) % SLOTS;
        size = (unsigned int)(state >> 23) + 1;
        op = (unsigned int)(state >> 21) % 3;
        if (slots[slot] && !valid(slot)) return 1;
        if (slots[slot] && op == 0) {
            free(slots[slot]); slots[slot] = NULL; lengths[slot] = 0;
        } else {
            if (slots[slot]) {
                p = realloc(slots[slot], size);
                if (!p) return 2;
                for (i = 0; i < lengths[slot] && i < size; i++)
                    if (p[i] != values[slot]) return 3;
            } else {
                p = malloc(size);
                if (!p) return 4;
            }
            slots[slot] = p; lengths[slot] = size; values[slot] = step + 1000;
            for (i = 0; i < size; i++) p[i] = values[slot];
        }
        for (i = 0; i < SLOTS; i++) if (slots[i] && !valid(i)) return 5;
    }
    for (slot = 0; slot < SLOTS; slot++) free(slots[slot]);
    /* All blocks must coalesce back to the original full arena. */
    p = malloc(251UL * 64000UL - 11);
    if (!p || malloc(1) != NULL) return 6;
    p[0] = 77; p[251UL * 64000UL - 12] = 88;
    if (p[0] != 77 || p[251UL * 64000UL - 12] != 88) return 7;
    free(p);
    p = malloc(1);
    if (p) putchar('Y'); else putchar('N');
    free(p);
    putchar('\n');
    return 0;
}
