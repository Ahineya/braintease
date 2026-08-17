/* q_div-shaped wrapper: two 2-word structs in A0-A3, then a 4-scalar +
 * 2-out-pointer call (same as runtime q_div -> u32_div_q16). */
void putchar(int c);

static void yn(int c) {
    putchar(c ? 'Y' : 'N');
}

typedef struct {
    short integer;
    unsigned short frac;
} q16_16_t;

static int limb_u32_ge(unsigned short a_hi, unsigned short a_lo,
                       unsigned short b_hi, unsigned short b_lo) {
    if (a_hi > b_hi) return 1;
    if (a_hi < b_hi) return 0;
    return a_lo >= b_lo;
}

static void limb_u32_sub(unsigned short a_hi, unsigned short a_lo,
                         unsigned short b_hi, unsigned short b_lo,
                         unsigned short *r_hi, unsigned short *r_lo) {
    unsigned short brw = (a_lo < b_lo) ? 1 : 0;
    *r_lo = a_lo - b_lo;
    *r_hi = a_hi - b_hi - brw;
}

static void limb_u32_div_q16(unsigned short n_hi, unsigned short n_lo,
                             unsigned short d_hi, unsigned short d_lo,
                             unsigned short *q_hi, unsigned short *q_lo) {
    unsigned short r_hi = 0;
    unsigned short r_lo = 0;
    unsigned short quot_hi = 0;
    unsigned short quot_lo = 0;
    int i;

    for (i = 0; i < 48; i++) {
        unsigned short inbit = 0;
        if (i < 16) {
            inbit = (n_hi >> (15 - i)) & 1;
        } else if (i < 32) {
            inbit = (n_lo >> (31 - i)) & 1;
        }

        unsigned short r_msb = (r_hi >> 15) & 1;
        r_hi = (unsigned short)((r_hi << 1) | (r_lo >> 15));
        r_lo = (unsigned short)((r_lo << 1) | inbit);

        unsigned short qbit = 0;
        if (r_msb || limb_u32_ge(r_hi, r_lo, d_hi, d_lo)) {
            limb_u32_sub(r_hi, r_lo, d_hi, d_lo, &r_hi, &r_lo);
            qbit = 1;
        }

        quot_hi = (unsigned short)((quot_hi << 1) | (quot_lo >> 15));
        quot_lo = (unsigned short)((quot_lo << 1) | qbit);
    }

    *q_hi = quot_hi;
    *q_lo = quot_lo;
}

static q16_16_t local_q_div(q16_16_t a, q16_16_t b) {
    unsigned short q_hi;
    unsigned short q_lo;
    q16_16_t q_result;

    limb_u32_div_q16((unsigned short)a.integer, a.frac,
                     (unsigned short)b.integer, b.frac,
                     &q_hi, &q_lo);

    q_result.integer = (short)q_hi;
    q_result.frac = q_lo;
    return q_result;
}

int main() {
    q16_16_t a;
    q16_16_t b;
    q16_16_t r;

    a.integer = 10;
    a.frac = 0;
    b.integer = 4;
    b.frac = 0;
    r = local_q_div(a, b);
    yn(r.integer == 2);
    yn(r.frac == 0x8000);

    a.integer = 3;
    a.frac = 0;
    b.integer = 5;
    b.frac = 0;
    r = local_q_div(a, b);
    yn(r.integer == 0);
    yn(r.frac > 0x8000 && r.frac < 0xB000);

    a.integer = 1;
    a.frac = 0;
    b.integer = 2;
    b.frac = 0;
    r = local_q_div(a, b);
    yn(r.integer == 0);
    yn(r.frac == 0x8000);

    putchar('\n');
    return 0;
}
