void putchar(int c);

static void yn(int c) {
    putchar(c ? 'Y' : 'N');
}

/* Same limb restoring 128/64 as runtime/softfloat.c. Not named div128_64:
 * that symbol already exists in the runtime library. */
static unsigned long long limb_div128_64(unsigned long long n1, unsigned long long n0,
                                         unsigned long long d, unsigned long long *rem) {
    unsigned long r1;
    unsigned long r0;
    unsigned long n1h;
    unsigned long n0h;
    unsigned long d1;
    unsigned long d0;
    unsigned long q1;
    unsigned long q0;
    int i;

    r1 = (unsigned long)(n1 >> 32);
    r0 = (unsigned long)n1;
    n1h = (unsigned long)(n0 >> 32);
    n0h = (unsigned long)n0;
    d1 = (unsigned long)(d >> 32);
    d0 = (unsigned long)d;
    q1 = 0;
    q0 = 0;

    for (i = 0; i < 64; i++) {
        unsigned long msb;
        unsigned long bit;
        unsigned long carry;
        unsigned long nr1;
        unsigned long nr0;
        unsigned long br;

        msb = r1 >> 31;
        bit = n1h >> 31;
        carry = n0h >> 31;
        n0h = n0h + n0h;
        n1h = (n1h + n1h) | carry;
        carry = r0 >> 31;
        nr0 = (r0 + r0) | bit;
        nr1 = (r1 + r1) | carry;
        carry = q0 >> 31;
        q0 = q0 + q0;
        q1 = (q1 + q1) | carry;
        if (msb || nr1 > d1 || (nr1 == d1 && nr0 >= d0)) {
            br = nr0 < d0;
            r0 = nr0 - d0;
            r1 = nr1 - d1 - br;
            q0 = q0 | 1UL;
        } else {
            r0 = nr0;
            r1 = nr1;
        }
    }
    *rem = ((unsigned long long)r1 << 32) | (unsigned long long)r0;
    return ((unsigned long long)q1 << 32) | (unsigned long long)q0;
}

int main() {
    unsigned long long rem;
    unsigned long long q;
    unsigned long long sig_a;
    unsigned long long sig_b;
    unsigned long x;
    unsigned long y;
    int i;

    x = 0x80000000UL;
    yn((x >> 31) == 1UL);
    x = 0x40000000UL;
    yn((x >> 31) == 0UL);
    x = 1UL;
    for (i = 0; i < 10; i++) {
        x = x + x;
    }
    yn(x == 1024UL);
    x = 0x80000000UL;
    y = x + x;
    yn(y == 0UL);
    x = 0xC0000000UL;
    y = 0x80000000UL;
    yn(x > y);
    yn((0x80000000UL) >= (0x80000000UL));

    sig_a = 0x18000000000000ULL;
    sig_b = 0x10000000000000ULL;
    yn((sig_a >> 3) == 0x3000000000000ULL);
    yn((sig_a << 61) == 0ULL);
    yn((sig_a >> 32) == 0x180000UL);

    q = limb_div128_64(0ULL, 10ULL, 2ULL, &rem);
    yn(q == 5ULL);
    yn(rem == 0ULL);

    q = limb_div128_64(0ULL, 8ULL, 2ULL, &rem);
    yn(q == 4ULL);
    yn(rem == 0ULL);

    q = limb_div128_64(1ULL, 0ULL, 2ULL, &rem);
    yn(q == 0x8000000000000000ULL);
    yn(rem == 0ULL);

    q = limb_div128_64(sig_a >> 3, sig_a << 61, sig_b, &rem);
    yn(q == 0x3000000000000000ULL);
    yn(rem == 0ULL);

    sig_a = 0x14000000000000ULL;
    sig_b = 0x10000000000000ULL;
    q = limb_div128_64(sig_a >> 3, sig_a << 61, sig_b, &rem);
    yn(q == 0x2800000000000000ULL);
    yn(rem == 0ULL);

    sig_a = 0x12000000000000ULL;
    sig_b = 0x18000000000000ULL;
    q = limb_div128_64(sig_a >> 3, sig_a << 61, sig_b, &rem);
    yn(q == 0x1800000000000000ULL);
    yn(rem == 0ULL);

    putchar('\n');
    return 0;
}
