void putchar(int c);

static void yn(int c) {
    putchar(c ? 'Y' : 'N');
}

int main() {
    unsigned long q1;
    unsigned long q0;
    unsigned long long q;
    unsigned long x;
    unsigned long y;
    int i;
    int ok;

    q1 = 0x30000000UL;
    q0 = 0UL;
    q = ((unsigned long long)q1 << 32) | (unsigned long long)q0;
    yn(q == 0x3000000000000000ULL);

    q0 = 0x80000000UL;
    q = ((unsigned long long)q1 << 32) | (unsigned long long)q0;
    yn(q == 0x3000000080000000ULL);

    q0 = 0xE0008000UL;
    q = ((unsigned long long)q1 << 32) | (unsigned long long)q0;
    yn(q == 0x30000000E0008000ULL);

    x = 0x80000000UL;
    ok = 1;
    for (i = 0; i < 64; i++) {
        y = x >> 31;
        if (y != 1UL) {
            ok = 0;
        }
    }
    yn(ok);

    x = 0x7FFFFFFFUL;
    ok = 1;
    for (i = 0; i < 64; i++) {
        y = x >> 31;
        if (y != 0UL) {
            ok = 0;
        }
    }
    yn(ok);

    q0 = 0;
    q1 = 0;
    for (i = 0; i < 64; i++) {
        unsigned long carry;
        unsigned long bit;
        bit = (i >= 60) ? 1UL : 0UL;
        carry = q0 >> 31;
        q0 = q0 + q0;
        q1 = (q1 + q1) | carry;
        q0 = q0 | bit;
    }
    yn(q0 == 15UL);
    yn(q1 == 0UL);

    putchar('\n');
    return 0;
}
