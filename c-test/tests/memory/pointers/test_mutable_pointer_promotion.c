#include <stdint.h>
#include <stdio.h>

static unsigned globals[8];

/* Two cursor updates per iteration model unpacking serialized octets. */
static unsigned *unpack(unsigned *out, unsigned count, unsigned word) {
    while (count != 0) {
        *out++ = word & 255U;
        *out++ = (word >> 8) & 255U;
        --count;
        word += 257U;
    }
    return out;
}

static unsigned *choose(unsigned *first, unsigned *second, unsigned count) {
    unsigned *cursor = first;
    while (count != 0) {
        if (count == 3) cursor = second;
        if (count == 1) return cursor;
        ++cursor;
        --count;
    }
    return cursor;
}

static void replace(unsigned **slot, unsigned *replacement) { *slot = replacement; }
static unsigned *advance_escaped(unsigned *cursor, unsigned *replacement) {
    replace(&cursor, replacement);
    return ++cursor;
}
static unsigned *advance_volatile(unsigned *cursor) {
    unsigned *volatile changing = cursor;
    changing++;
    changing++;
    return changing;
}

int main(void) {
    unsigned stack[8];
    unsigned *edge = (unsigned *)0x0003f9feUL;
    unsigned *end;
    end = unpack(globals, 3, 0xabcdU);
    if (end == globals + 6 && globals[0] == 205 && globals[1] == 171
        && globals[4] == 207 && globals[5] == 173) putchar('Y'); else putchar('N');
    end = unpack(stack, 3, 0x1234U);
    if (end == stack + 6 && stack[0] == 52 && stack[1] == 18
        && stack[4] == 54 && stack[5] == 20) putchar('Y'); else putchar('N');
    end = unpack(edge, 3, 0xfeffU);
    if ((uintptr_t)end == 0x00040004UL && edge[0] == 255 && edge[1] == 254
        && edge[2] == 0 && edge[3] == 0 && edge[4] == 1 && edge[5] == 1) putchar('Y'); else putchar('N');
    if (unpack((unsigned *)0, 0, 123) == (unsigned *)0) putchar('Y'); else putchar('N');
    if (choose(globals, stack, 0) == globals) putchar('Y'); else putchar('N');
    if (choose(globals, stack, 4) == stack + 2) putchar('Y'); else putchar('N');
    if (choose(stack, edge, 3) == edge + 2) putchar('Y'); else putchar('N');
    if (advance_escaped(globals, stack) == stack + 1) putchar('Y'); else putchar('N');
    if (advance_volatile(edge) == edge + 2) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
