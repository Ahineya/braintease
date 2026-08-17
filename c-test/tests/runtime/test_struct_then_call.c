/* Split local_q_div into pieces: struct args, nested 6-arg call, struct return. */
void putchar(int c);

static void yn(int c) {
    putchar(c ? 'Y' : 'N');
}

typedef struct {
    short integer;
    unsigned short frac;
} q16_16_t;

static unsigned short g_a, g_b, g_c, g_d;
static unsigned short g_qhi, g_qlo;
static short g_ret_i;
static unsigned short g_ret_f;

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

static q16_16_t read_structs(q16_16_t a, q16_16_t b) {
    g_a = (unsigned short)a.integer;
    g_b = a.frac;
    g_c = (unsigned short)b.integer;
    g_d = b.frac;
    return a;
}

static q16_16_t structs_then_call(q16_16_t a, q16_16_t b) {
    unsigned short q_hi;
    unsigned short q_lo;
    q16_16_t r;

    take6((unsigned short)a.integer, a.frac,
          (unsigned short)b.integer, b.frac,
          &q_hi, &q_lo);

    g_qhi = q_hi;
    g_qlo = q_lo;
    r.integer = (short)q_hi;
    r.frac = q_lo;
    g_ret_i = r.integer;
    g_ret_f = r.frac;
    return r;
}

static q16_16_t make_pair(short i, unsigned short f) {
    q16_16_t r;
    r.integer = i;
    r.frac = f;
    return r;
}

int main() {
    q16_16_t a;
    q16_16_t b;
    q16_16_t r;

    a.integer = 10;
    a.frac = 0;
    b.integer = 4;
    b.frac = 0;

    /* 1-4: struct args arrive intact, no nested call */
    g_a = g_b = g_c = g_d = 0xFFFF;
    r = read_structs(a, b);
    yn(g_a == 10);
    yn(g_b == 0);
    yn(g_c == 4);
    yn(g_d == 0);

    /* 5-8: fields forwarded into 4-scalar + 2-outptr call */
    g_a = g_b = g_c = g_d = 0xFFFF;
    r = structs_then_call(a, b);
    yn(g_a == 10);
    yn(g_b == 0);
    yn(g_c == 4);
    yn(g_d == 0);

    /* 9-12: out-params and returned struct */
    yn(g_qhi == 2);
    yn(g_qlo == 0x8000);
    yn(g_ret_i == 2);
    yn(g_ret_f == 0x8000);

    /* 13-16: caller-side returned struct */
    yn(r.integer == 2);
    yn(r.frac == 0x8000);

    r = make_pair(2, 0x8000);
    yn(r.integer == 2);
    yn(r.frac == 0x8000);

    putchar('\n');
    return 0;
}
