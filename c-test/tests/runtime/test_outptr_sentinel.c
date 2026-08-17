/* Same sentinel checks, but inside a 4-register-arg function like q_div. */
void putchar(int c);

static void yn(int c) {
    putchar(c ? 'Y' : 'N');
}

static unsigned short g_hi, g_lo;

static void take6(unsigned short a, unsigned short b,
                  unsigned short c, unsigned short d,
                  unsigned short *out_hi, unsigned short *out_lo) {
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    *out_hi = 2;
    *out_lo = 0x8000;
}

static void run(unsigned short a, unsigned short b,
                unsigned short c, unsigned short d) {
    unsigned short q_hi;
    unsigned short q_lo;
    q_hi = 0xDEAD;
    q_lo = 0xBEEF;
    take6(a, b, c, d, &q_hi, &q_lo);
    g_hi = q_hi;
    g_lo = q_lo;
}

int main() {
    run(10, 0, 4, 0);

    yn(g_hi == 2);
    yn(g_hi == 0xDEAD);
    yn(g_hi == 0x8000);
    yn(g_hi == 0);
    yn(g_hi == 10);

    yn(g_lo == 0x8000);
    yn(g_lo == 0xBEEF);
    yn(g_lo == 2);
    yn(g_lo == 0);
    yn(g_lo == 10);

    putchar('\n');
    return 0;
}
