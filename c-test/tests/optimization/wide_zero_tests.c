#include <stdio.h>

static volatile unsigned long inputs[] = {
    0UL, 1UL, 65535UL, 65536UL, 0x0001ffffUL,
    0x7fffffffUL, 0x80000000UL, 0xffffffffUL
};
static volatile int side_effect;
static int storage[2];

static unsigned test(unsigned long value, int expected) {
    int eq = value == 0UL;
    int ne = value != 0UL;
    int left_eq = 0UL == value;
    int left_ne = 0UL != value;
    _Bool boolean = value;
    int branch = 0;
    if (value) branch = 1;
    if (!value) branch += 2;
    /* Keep the predicate live after its branch and verify normalization. */
    if (ne) side_effect = 7;
    else side_effect = 9;
    return eq == !expected && ne == expected && left_eq == !expected
        && left_ne == expected && boolean == expected
        && branch == (expected ? 1 : 2)
        && side_effect == (expected ? 7 : 9);
}

int main(void) {
    unsigned i;
    for (i = 0; i < 8; ++i) {
        if (test(inputs[i], i != 0)) putchar('Y'); else putchar('N');
    }
    {
        volatile long signed_value = -2147483647L - 1L;
        if (signed_value && signed_value != 0L && !(signed_value == 0L)) putchar('Y'); else putchar('N');
    }
    {
        int *volatile pointer = storage;
        _Bool boolean = pointer;
        if (pointer && pointer != 0 && boolean && pointer == storage) putchar('Y'); else putchar('N');
        pointer = 0;
        boolean = pointer;
        if (!pointer && pointer == 0 && !boolean) putchar('Y'); else putchar('N');
        pointer = storage + 1;
        if (pointer && 0 != pointer && pointer != storage) putchar('Y'); else putchar('N');
    }
    putchar('\n');
    return 0;
}
