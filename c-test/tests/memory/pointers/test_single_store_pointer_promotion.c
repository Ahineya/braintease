#include <stdint.h>
#include <stdio.h>

static long global_values[6];
static long *saved;

/* Reuse a pointer parameter across enough stores to retain a real call. */
static long *fill(long *out) {
    long *alias = out;
    alias[0] = 0x12345678L;
    alias[1] = -170910304L;
    alias[2] = 65536L;
    alias[3] = -1L;
    alias[4] = 0L;
    alias[5] = 0x7fffffffL;
    return out + 6;
}

static int valid(long *values) {
    return values[0] == 0x12345678L && values[1] == -170910304L
        && values[2] == 65536L && values[3] == -1L
        && values[4] == 0L && values[5] == 0x7fffffffL;
}

static void replace(long **slot, long *replacement) { *slot = replacement; }
static long *identity(long *value) { return value; }

int main(void) {
    long stack_values[6];
    long *heap_edge = (long *)0x0003f9ffUL;
    long *escaped = global_values;
    long *volatile changing = global_values;
    long *returned;
    long *end;

    end = fill(global_values);
    if (valid(global_values) && end == global_values + 6) putchar('Y'); else putchar('N');
    end = fill(stack_values);
    if (valid(stack_values) && end == stack_values + 6) putchar('Y'); else putchar('N');
    end = fill(heap_edge);
    if (valid(heap_edge) && (uintptr_t)end == 0x0004000bUL) putchar('Y'); else putchar('N');

    returned = identity(stack_values + 2);
    saved = returned;
    if (*returned == 65536L && saved == stack_values + 2) putchar('Y'); else putchar('N');
    replace(&escaped, stack_values + 1);
    if (*escaped == -170910304L) putchar('Y'); else putchar('N');
    changing = stack_values + 3;
    if (*changing == -1L) putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
