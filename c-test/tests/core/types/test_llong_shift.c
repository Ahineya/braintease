void putchar(int c);

int main() {
    unsigned long long u;
    long long s;
    int n;

    u = 1ULL;
    if ((u << 3) == 8ULL) putchar('Y'); else putchar('N');
    if ((u << 16) == 65536ULL) putchar('Y'); else putchar('N');
    if ((u << 32) == 4294967296ULL) putchar('Y'); else putchar('N');
    if ((u << 48) == 281474976710656ULL) putchar('Y'); else putchar('N');
    if ((u << 62) == 4611686018427387904ULL) putchar('Y'); else putchar('N');

    u = 0x123456789ABCDEF0ULL;
    if ((u >> 32) == 0x12345678ULL) putchar('Y'); else putchar('N');
    if ((u >> 16) == 0x123456789ABCULL) putchar('Y'); else putchar('N');

    n = 3;
    u = 5ULL;
    if ((u << n) == 40ULL) putchar('Y'); else putchar('N');
    n = 32;
    if ((u >> n) == 0ULL) putchar('Y'); else putchar('N');
    u = 4294967296ULL;
    n = 32;
    if ((u >> n) == 1ULL) putchar('Y'); else putchar('N');

    s = -8;
    if ((s >> 1) == -4) putchar('Y'); else putchar('N');
    s = -9223372036854775807LL - 1;
    if ((s >> 63) == -1) putchar('Y'); else putchar('N');

    putchar('\n');
    return 0;
}
