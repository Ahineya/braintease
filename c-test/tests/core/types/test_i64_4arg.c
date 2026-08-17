void putchar(int c);

static void yn(int c) {
    putchar(c ? 'Y' : 'N');
}

/* Same ABI as runtime div128_64: I64, I64, I64, pointer. */
static unsigned long long take_b(unsigned long long a, unsigned long long b,
                                 unsigned long long c, unsigned long long *out) {
    *out = c;
    return b;
}

static unsigned long long take_a(unsigned long long a, unsigned long long b,
                                 unsigned long long c, unsigned long long *out) {
    *out = b;
    return a;
}

static unsigned long long take_c(unsigned long long a, unsigned long long b,
                                 unsigned long long c, unsigned long long *out) {
    *out = a;
    return c;
}

int main() {
    unsigned long long o;
    unsigned long long r;

    r = take_a(1ULL, 10ULL, 2ULL, &o);
    yn(r == 1ULL);
    yn(o == 10ULL);

    r = take_b(1ULL, 10ULL, 2ULL, &o);
    yn(r == 10ULL);
    yn(o == 2ULL);

    r = take_c(1ULL, 10ULL, 2ULL, &o);
    yn(r == 2ULL);
    yn(o == 1ULL);

    r = take_a(0x3000000000000ULL, 0ULL, 0x10000000000000ULL, &o);
    yn(r == 0x3000000000000ULL);
    yn(o == 0ULL);

    r = take_b(0x3000000000000ULL, 0ULL, 0x10000000000000ULL, &o);
    yn(r == 0ULL);
    yn(o == 0x10000000000000ULL);

    putchar('\n');
    return 0;
}
