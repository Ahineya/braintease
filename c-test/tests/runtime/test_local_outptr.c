/* Out-pointers to LOCALS, not to the caller's variables.
 * from_4reg passed main's &hi/&lo and passed; structs_then_call's
 * &q_hi/&q_lo did not. */
void putchar(int c);

static void yn(int c) {
    putchar(c ? 'Y' : 'N');
}

typedef struct {
    short integer;
    unsigned short frac;
} q16_16_t;

static unsigned short g_hi, g_lo;

static void write_pair(unsigned short *out_hi, unsigned short *out_lo) {
    *out_hi = 2;
    *out_lo = 0x8000;
}

static void four_scalars_local_out(unsigned short a, unsigned short b,
                                   unsigned short c, unsigned short d) {
    unsigned short q_hi;
    unsigned short q_lo;
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    write_pair(&q_hi, &q_lo);
    g_hi = q_hi;
    g_lo = q_lo;
}

static void two_structs_local_out(q16_16_t a, q16_16_t b) {
    unsigned short q_hi;
    unsigned short q_lo;
    (void)a;
    (void)b;
    write_pair(&q_hi, &q_lo);
    g_hi = q_hi;
    g_lo = q_lo;
}

static void two_structs_direct_store(q16_16_t a, q16_16_t b) {
    unsigned short q_hi;
    unsigned short q_lo;
    unsigned short *p_hi;
    unsigned short *p_lo;
    (void)a;
    (void)b;
    p_hi = &q_hi;
    p_lo = &q_lo;
    *p_hi = 2;
    *p_lo = 0x8000;
    g_hi = q_hi;
    g_lo = q_lo;
}

static void no_params_local_out(void) {
    unsigned short q_hi;
    unsigned short q_lo;
    write_pair(&q_hi, &q_lo);
    g_hi = q_hi;
    g_lo = q_lo;
}

int main() {
    q16_16_t a;
    q16_16_t b;

    a.integer = 10;
    a.frac = 0;
    b.integer = 4;
    b.frac = 0;

    g_hi = g_lo = 0;
    no_params_local_out();
    yn(g_hi == 2);
    yn(g_lo == 0x8000);

    g_hi = g_lo = 0;
    four_scalars_local_out(10, 0, 4, 0);
    yn(g_hi == 2);
    yn(g_lo == 0x8000);

    g_hi = g_lo = 0;
    two_structs_direct_store(a, b);
    yn(g_hi == 2);
    yn(g_lo == 0x8000);

    g_hi = g_lo = 0;
    two_structs_local_out(a, b);
    yn(g_hi == 2);
    yn(g_lo == 0x8000);

    putchar('\n');
    return 0;
}
