/* 4 register scalars + 2 STACK out-pointers to THIS function's locals.
 * from_4reg passed the caller's pointers (worked). write_pair put both
 * pointers in A0-A3 (worked). q_div puts pointers on the stack. */
void putchar(int c);

static void yn(int c) {
    putchar(c ? 'Y' : 'N');
}

typedef struct {
    short integer;
    unsigned short frac;
} q16_16_t;

static unsigned short g_hi, g_lo;
static unsigned short g_a, g_b, g_c, g_d;

static void take6(unsigned short a, unsigned short b,
                  unsigned short c, unsigned short d,
                  unsigned short *out_hi, unsigned short *out_lo) {
    g_a = a;
    g_b = b;
    g_c = c;
    g_d = d;
    *out_hi = 2;
    *out_lo = 0x8000;
}

static void four_scalars_stack_local(unsigned short a, unsigned short b,
                                     unsigned short c, unsigned short d) {
    unsigned short q_hi;
    unsigned short q_lo;
    take6(a, b, c, d, &q_hi, &q_lo);
    g_hi = q_hi;
    g_lo = q_lo;
}

static void two_structs_stack_local(q16_16_t a, q16_16_t b) {
    unsigned short q_hi;
    unsigned short q_lo;
    take6((unsigned short)a.integer, a.frac,
          (unsigned short)b.integer, b.frac,
          &q_hi, &q_lo);
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

    g_a = g_b = g_c = g_d = 0;
    g_hi = g_lo = 0;
    four_scalars_stack_local(10, 0, 4, 0);
    yn(g_a == 10);
    yn(g_b == 0);
    yn(g_c == 4);
    yn(g_d == 0);
    yn(g_hi == 2);
    yn(g_lo == 0x8000);

    g_a = g_b = g_c = g_d = 0;
    g_hi = g_lo = 0;
    two_structs_stack_local(a, b);
    yn(g_a == 10);
    yn(g_b == 0);
    yn(g_c == 4);
    yn(g_d == 0);
    yn(g_hi == 2);
    yn(g_lo == 0x8000);

    putchar('\n');
    return 0;
}
