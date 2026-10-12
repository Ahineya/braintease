#include <stdio.h>

static int divide(int a, int b, int quotient, int remainder) {
    return a / b == quotient && a % b == remainder;
}

static int shift(int a, unsigned int count, int expected) {
    return a >> count == expected;
}

int main(void) {
    int minimum = -32767 - 1;
    int value = -7;
    unsigned int unsigned_value = 65529U;
    int count;

    if (!divide(-7, 3, -2, -1) || !divide(7, -3, -2, 1) ||
        !divide(-7, -3, 2, -1) || !divide(7, 3, 2, 1) ||
        !divide(minimum, 7, -4681, -1) ||
        !divide(minimum, -3, 10922, -2) ||
        !divide(minimum, minimum, 1, 0) || !divide(0, -3, 0, 0)) {
        puts("division failed");
        return 1;
    }

    /* Literal RHS exercises the immediate lowering path. */
    if (value / 3 != -2 || value % 3 != -1 ||
        value / -3 != 2 || value % -3 != -1 ||
        value >> 1 != -4 || value >> 0 != -7 || value >> 15 != -1) {
        puts("immediate failed");
        return 1;
    }

    for (count = 0; count < 16; ++count) {
        if (!shift(-1, count, -1) || !shift(0, count, 0) ||
            !shift(minimum, count, -(32768U >> count))) {
            puts("shift failed");
            return 1;
        }
    }
    if (!shift(-7, 1, -4) || !shift(7, 1, 3) ||
        unsigned_value / 3U != 21843U || unsigned_value % 3U != 0U ||
        unsigned_value >> 1 != 32764U) {
        puts("unsigned failed");
        return 1;
    }

    value /= 3;
    if (value != -2) return 1;
    value = -7;
    value %= -3;
    if (value != -1) return 1;
    value = -7;
    value >>= 1U;
    if (value != -4) return 1;

    puts("signed arithmetic ok");
    return 0;
}
