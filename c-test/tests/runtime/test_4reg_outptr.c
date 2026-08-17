/* Callee has 4 register args (A0-A3 live) and calls a helper with
 * those 4 scalars plus two out-pointers — the q_div -> u32_div_q16 ABI.
 * Each received word is checked against the value that should have been
 * forwarded, not against leftover locals. */
void putchar(int c);

static void yn(int c) {
    putchar(c ? 'Y' : 'N');
}

static unsigned short g_a, g_b, g_c, g_d;

static void take6(unsigned short a, unsigned short b,
                  unsigned short c, unsigned short d,
                  unsigned short *out_hi, unsigned short *out_lo) {
    g_a = a;
    g_b = b;
    g_c = c;
    g_d = d;
    *out_hi = a;
    *out_lo = b;
}

static void from_4reg(unsigned short a, unsigned short b,
                      unsigned short c, unsigned short d,
                      unsigned short *out_hi, unsigned short *out_lo) {
    take6(a, b, c, d, out_hi, out_lo);
}

int main() {
    unsigned short hi;
    unsigned short lo;

    g_a = g_b = g_c = g_d = 0xFFFF;
    from_4reg(10, 20, 30, 40, &hi, &lo);
    yn(g_a == 10);
    yn(g_b == 20);
    yn(g_c == 30);
    yn(g_d == 40);
    yn(hi == 10);
    yn(lo == 20);

    g_a = g_b = g_c = g_d = 0xFFFF;
    from_4reg(3, 0, 5, 0, &hi, &lo);
    yn(g_a == 3);
    yn(g_b == 0);
    yn(g_c == 5);
    yn(g_d == 0);

    putchar('\n');
    return 0;
}
