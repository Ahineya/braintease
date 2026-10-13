#include <stdio.h>

static volatile unsigned int limits[3] = {0, 3, 5};

static unsigned int rows(unsigned int count, unsigned int limit, unsigned int stop) {
    unsigned int total = 7;
    unsigned int carry = 123;
    unsigned int row;
    for (row = 0; row < count; ++row) {
        unsigned int scratch = 555;
        unsigned int i;
        for (i = 0; i < limit; ++i) {
            scratch = (row + 1) * 10 + i;
            if (i == stop) break;
            total += scratch;
            carry = scratch;
        }
        scratch = 900;
        total += scratch;
    }
    return total + carry;
}

static unsigned int bump(unsigned int *p, unsigned int amount) {
    *p += amount;
    return *p;
}

static unsigned int escaped(unsigned int count) {
    unsigned int value = 17;
    unsigned int (*volatile action)(unsigned int *, unsigned int) = bump;
    unsigned int i;
    for (i = 0; i < count; ++i) action(&value, i);
    return value;
}

static unsigned int reused_condition(unsigned int count) {
    unsigned int answer = 0;
    unsigned int i;
    for (i = 0; i != count; ++i) {
        unsigned int equal = i == count / 2;
        if (equal) answer += 5;
        else answer += 2;
        answer += equal;
    }
    return answer;
}

int main(void) {
    if (rows(limits[0], 3, 99) == 130) putchar('Y'); else putchar('N');
    if (rows(2, limits[0], 99) == 1930) putchar('Y'); else putchar('N');
    if (rows(2, limits[1], 99) == 1925) putchar('Y'); else putchar('N');
    if (rows(2, limits[2], 1) == 1857) putchar('Y'); else putchar('N');
    if (escaped(limits[1] + 1) == 23) putchar('Y'); else putchar('N');
    if (reused_condition(limits[2]) == 14 && reused_condition(limits[0]) == 0)
        putchar('Y');
    else putchar('N');
    putchar('\n');
    return 0;
}
