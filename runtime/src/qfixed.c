// Q16.16 Fixed-Point Implementation for Ripple VM
// Ripple int/unsigned int are 16-bit; all 32-bit math is done with 16-bit pairs.

#include "qfixed.h"

/* 16x16 unsigned multiply -> 32-bit (hi:lo), using 8-bit partial products
 * so that each multiply fits in a 16-bit cell. */
static void umul16(unsigned short a, unsigned short b,
                   unsigned short *hi, unsigned short *lo) {
    unsigned short a0 = a & 0x00FF;
    unsigned short a1 = (a >> 8) & 0x00FF;
    unsigned short b0 = b & 0x00FF;
    unsigned short b1 = (b >> 8) & 0x00FF;

    unsigned short p00 = a0 * b0;
    unsigned short p01 = a0 * b1;
    unsigned short p10 = a1 * b0;
    unsigned short p11 = a1 * b1;

    unsigned short mid = p01 + p10;
    unsigned short mid_carry = (mid < p01) ? 1 : 0;
    unsigned short mid_lo8 = mid & 0x00FF;
    unsigned short mid_hi8 = (mid >> 8) & 0x00FF;

    unsigned short t_lo = p00 + (mid_lo8 << 8);
    unsigned short c = (t_lo < p00) ? 1 : 0;
    unsigned short t_hi = p11 + mid_hi8 + (mid_carry << 8) + c;

    *lo = t_lo;
    *hi = t_hi;
}

static int u32_ge(unsigned short a_hi, unsigned short a_lo,
                  unsigned short b_hi, unsigned short b_lo) {
    if (a_hi > b_hi) return 1;
    if (a_hi < b_hi) return 0;
    return a_lo >= b_lo;
}

static void u32_sub(unsigned short a_hi, unsigned short a_lo,
                    unsigned short b_hi, unsigned short b_lo,
                    unsigned short *r_hi, unsigned short *r_lo) {
    unsigned short brw = (a_lo < b_lo) ? 1 : 0;
    *r_lo = a_lo - b_lo;
    *r_hi = a_hi - b_hi - brw;
}

/* 48-bit dividend (n << 16) / 32-bit divisor -> 32-bit quotient. */
static void u32_div_q16(unsigned short n_hi, unsigned short n_lo,
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
        if (r_msb || u32_ge(r_hi, r_lo, d_hi, d_lo)) {
            u32_sub(r_hi, r_lo, d_hi, d_lo, &r_hi, &r_lo);
            qbit = 1;
        }

        quot_hi = (unsigned short)((quot_hi << 1) | (quot_lo >> 15));
        quot_lo = (unsigned short)((quot_lo << 1) | qbit);
    }

    *q_hi = quot_hi;
    *q_lo = quot_lo;
}

q16_16_t q_add(q16_16_t a, q16_16_t b) {
    unsigned short frac_sum = a.frac + b.frac;
    unsigned short carry = (frac_sum < a.frac) ? 1 : 0;
    q16_16_t result;
    result.frac = frac_sum;
    result.integer = (short)((unsigned short)a.integer + (unsigned short)b.integer + carry);
    return result;
}

q16_16_t q_sub(q16_16_t a, q16_16_t b) {
    q16_16_t result;
    unsigned short brw = (a.frac < b.frac) ? 1 : 0;
    result.frac = a.frac - b.frac;
    result.integer = (short)((unsigned short)a.integer - (unsigned short)b.integer - brw);
    return result;
}

q16_16_t q_neg(q16_16_t x) {
    q16_16_t result;
    result.frac = (unsigned short)(0 - x.frac);
    if (x.frac == 0) {
        result.integer = (short)(0 - x.integer);
    } else {
        result.integer = (short)(~x.integer);
    }
    return result;
}

q16_16_t q_abs(q16_16_t x) {
    if (x.integer < 0) {
        return q_neg(x);
    }
    return x;
}

q16_16_t q_mul(q16_16_t a, q16_16_t b) {
    int sign = 0;
    if (a.integer < 0) {
        sign = sign ^ 1;
        a = q_neg(a);
    }
    if (b.integer < 0) {
        sign = sign ^ 1;
        b = q_neg(b);
    }

    unsigned short p0h, p0l, p1h, p1l, p2h, p2l, p3h, p3l;
    umul16(a.frac, b.frac, &p0h, &p0l);
    umul16((unsigned short)a.integer, b.frac, &p1h, &p1l);
    umul16((unsigned short)b.integer, a.frac, &p2h, &p2l);
    umul16((unsigned short)a.integer, (unsigned short)b.integer, &p3h, &p3l);

    /* result = (p3 << 16) + p1 + p2 + (p0 >> 16) */
    unsigned short frac_lo = p1l + p2l;
    unsigned short c = (frac_lo < p1l) ? 1 : 0;
    unsigned short frac_hi = p1h + p2h + c;

    unsigned short t = frac_lo + p0h;
    c = (t < frac_lo) ? 1 : 0;
    frac_lo = t;
    frac_hi = frac_hi + c;

    q16_16_t result;
    result.frac = frac_lo;
    result.integer = (short)(p3l + frac_hi);

    if (sign) {
        result = q_neg(result);
    }
    return result;
}

q16_16_t q_div(q16_16_t a, q16_16_t b) {
    if (b.integer == 0 && b.frac == 0) {
        return (q16_16_t){0x7FFF, 0xFFFF};
    }

    int sign = 0;
    if (a.integer < 0) {
        sign = sign ^ 1;
        a = q_neg(a);
    }
    if (b.integer < 0) {
        sign = sign ^ 1;
        b = q_neg(b);
    }

    unsigned short q_hi, q_lo;
    u32_div_q16((unsigned short)a.integer, a.frac,
                (unsigned short)b.integer, b.frac,
                &q_hi, &q_lo);

    q16_16_t q_result;
    q_result.integer = (short)q_hi;
    q_result.frac = q_lo;

    if (sign) {
        q_result = q_neg(q_result);
    }
    return q_result;
}

int q_eq(q16_16_t a, q16_16_t b) {
    return (a.integer == b.integer) && (a.frac == b.frac);
}

int q_lt(q16_16_t a, q16_16_t b) {
    if (a.integer < b.integer) return 1;
    if (a.integer > b.integer) return 0;
    return a.frac < b.frac;
}

int q_le(q16_16_t a, q16_16_t b) {
    return q_lt(a, b) || q_eq(a, b);
}

int q_gt(q16_16_t a, q16_16_t b) {
    return q_lt(b, a);
}

int q_ge(q16_16_t a, q16_16_t b) {
    return !q_lt(a, b);
}

q16_16_t q_from_int(int x) {
    q16_16_t result;
    result.integer = (short)x;
    result.frac = 0;
    return result;
}

int q_to_int(q16_16_t x) {
    return x.integer;
}

int q_to_int_round(q16_16_t x) {
    if (x.frac >= 0x8000) {
        return x.integer + 1;
    }
    return x.integer;
}

q16_16_t q_sqrt(q16_16_t x) {
    if (x.integer < 0) {
        return Q16_16_ZERO;
    }

    if (x.integer == 0 && x.frac == 0) {
        return Q16_16_ZERO;
    }

    q16_16_t guess;
    if (x.integer >= 1) {
        guess.integer = x.integer >> 1;
        guess.frac = x.frac >> 1;
        if (x.integer & 1) {
            guess.frac = guess.frac | 0x8000;
        }
    } else {
        guess = Q16_16_HALF;
    }

    for (int i = 0; i < 8; i++) {
        q16_16_t x_over_guess = q_div(x, guess);
        q16_16_t sum = q_add(guess, x_over_guess);

        q16_16_t next;
        next.integer = sum.integer >> 1;
        next.frac = sum.frac >> 1;
        if (sum.integer & 1) {
            next.frac = next.frac | 0x8000;
        }

        if (q_eq(next, guess)) {
            break;
        }

        guess = next;
    }

    return guess;
}

q16_16_t q_reciprocal(q16_16_t x) {
    if (x.integer == 0 && x.frac == 0) {
        return (q16_16_t){0x7FFF, 0xFFFF};
    }

    q16_16_t ax = q_abs(x);
    q16_16_t guess;
    if (ax.integer >= 2) {
        guess = Q16_16_HALF;
    } else if (ax.integer >= 1) {
        guess = Q16_16_ONE;
    } else {
        guess = (q16_16_t){2, 0};
    }

    q16_16_t two = (q16_16_t){2, 0};

    for (int i = 0; i < 8; i++) {
        q16_16_t x_guess = q_mul(x, guess);
        q16_16_t two_minus = q_sub(two, x_guess);
        q16_16_t next = q_mul(guess, two_minus);

        if (q_eq(next, guess)) {
            break;
        }

        guess = next;
    }

    return guess;
}

q16_16_t q_floor(q16_16_t x) {
    q16_16_t result;
    result.integer = x.integer;
    result.frac = 0;
    if (x.integer < 0 && x.frac > 0) {
        result.integer--;
    }
    return result;
}

q16_16_t q_ceil(q16_16_t x) {
    q16_16_t result;
    result.integer = x.integer;
    result.frac = 0;
    if (x.frac > 0) {
        result.integer++;
    }
    return result;
}

q16_16_t q_round(q16_16_t x) {
    q16_16_t result;
    result.integer = x.integer;
    result.frac = 0;
    if (x.frac >= 0x8000) {
        result.integer++;
    }
    return result;
}

q16_16_t q_min(q16_16_t a, q16_16_t b) {
    if (q_lt(a, b)) {
        return a;
    }
    return b;
}

q16_16_t q_max(q16_16_t a, q16_16_t b) {
    if (q_gt(a, b)) {
        return a;
    }
    return b;
}

q16_16_t q_clamp(q16_16_t x, q16_16_t min, q16_16_t max) {
    if (q_lt(x, min)) return min;
    if (q_gt(x, max)) return max;
    return x;
}

q16_16_t q_lerp(q16_16_t a, q16_16_t b, q16_16_t t) {
    q16_16_t diff = q_sub(b, a);
    q16_16_t scaled = q_mul(diff, t);
    return q_add(a, scaled);
}

q16_16_t q_sin(q16_16_t x) {
    q16_16_t two_pi = (q16_16_t){6, 0x487E};
    int n;

    /* Bound the reduction so a comparison/subtraction bug cannot hang. */
    n = 0;
    while (q_gt(x, Q16_16_PI) && n < 64) {
        x = q_sub(x, two_pi);
        n++;
    }
    n = 0;
    while (q_lt(x, q_neg(Q16_16_PI)) && n < 64) {
        x = q_add(x, two_pi);
        n++;
    }

    q16_16_t x2 = q_mul(x, x);
    q16_16_t x3 = q_mul(x2, x);
    q16_16_t x5 = q_mul(x3, x2);
    q16_16_t x7 = q_mul(x5, x2);

    q16_16_t c3 = q_div(Q16_16_ONE, q_from_int(6));
    q16_16_t c5 = q_div(Q16_16_ONE, q_from_int(120));
    q16_16_t c7 = q_div(Q16_16_ONE, q_from_int(5040));

    q16_16_t term1 = x;
    q16_16_t term2 = q_mul(x3, c3);
    q16_16_t term3 = q_mul(x5, c5);
    q16_16_t term4 = q_mul(x7, c7);

    q16_16_t result = term1;
    result = q_sub(result, term2);
    result = q_add(result, term3);
    result = q_sub(result, term4);

    return result;
}

q16_16_t q_cos(q16_16_t x) {
    q16_16_t two_pi = (q16_16_t){6, 0x487E};
    int n;

    n = 0;
    while (q_gt(x, Q16_16_PI) && n < 64) {
        x = q_sub(x, two_pi);
        n++;
    }
    n = 0;
    while (q_lt(x, q_neg(Q16_16_PI)) && n < 64) {
        x = q_add(x, two_pi);
        n++;
    }

    /* Taylor around 0: 1 - x^2/2 + x^4/24 - x^6/720.
     * Using sin(x+pi/2) is inaccurate at x=0 because that evaluates sin(pi/2). */
    q16_16_t x2 = q_mul(x, x);
    q16_16_t x4 = q_mul(x2, x2);
    q16_16_t x6 = q_mul(x4, x2);

    q16_16_t c2 = q_div(Q16_16_ONE, q_from_int(2));
    q16_16_t c4 = q_div(Q16_16_ONE, q_from_int(24));
    q16_16_t c6 = q_div(Q16_16_ONE, q_from_int(720));

    q16_16_t result = Q16_16_ONE;
    result = q_sub(result, q_mul(x2, c2));
    result = q_add(result, q_mul(x4, c4));
    result = q_sub(result, q_mul(x6, c6));
    return result;
}

q16_16_t q_tan(q16_16_t x) {
    return q_div(q_sin(x), q_cos(x));
}
