/* Same restoring divider as runtime/src/qfixed.c u32_div_q16.
 * Names are prefixed so they do not collide with the runtime object. */
void putchar(int c);

static void yn(int c) {
    putchar(c ? 'Y' : 'N');
}

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

int main() {
    unsigned short q_hi;
    unsigned short q_lo;

    /* 10/4 in Q16.16: n=10<<16, d=4<<16 -> 2.5 */
    limb_u32_div_q16(10, 0, 4, 0, &q_hi, &q_lo);
    yn(q_hi == 2);
    yn(q_lo == 0x8000);

    /* 3/5 -> 0.6, frac in (0x8000, 0xB000) */
    limb_u32_div_q16(3, 0, 5, 0, &q_hi, &q_lo);
    yn(q_hi == 0);
    yn(q_lo > 0x8000 && q_lo < 0xB000);

    /* 4/5 -> 0.8, frac in (0xB000, 0xE000) */
    limb_u32_div_q16(4, 0, 5, 0, &q_hi, &q_lo);
    yn(q_hi == 0);
    yn(q_lo > 0xB000 && q_lo < 0xE000);

    /* 1/2 -> 0.5 */
    limb_u32_div_q16(1, 0, 2, 0, &q_hi, &q_lo);
    yn(q_hi == 0);
    yn(q_lo == 0x8000);

    putchar('\n');
    return 0;
}
