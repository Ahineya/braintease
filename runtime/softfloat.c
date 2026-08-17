/* Software IEEE-754 binary32 / binary64 for Ripple.
 * Bit patterns are passed as unsigned long (f32) and unsigned long long (f64).
 * No hardware float types are used. */

#define F32_SIGN 0x80000000UL
#define F32_EXP  0x7F800000UL
#define F32_FRAC 0x007FFFFFUL
#define F32_QNAN 0x7FC00000UL
#define F32_INF  0x7F800000UL

#define F64_SIGN 0x8000000000000000ULL
#define F64_EXP  0x7FF0000000000000ULL
#define F64_FRAC 0x000FFFFFFFFFFFFFULL
#define F64_QNAN 0x7FF8000000000000ULL
#define F64_INF  0x7FF0000000000000ULL

static int clz32(unsigned long x) {
    int n;

    n = 0;
    if (x == 0) {
        return 32;
    }
    if ((x & 0xFFFF0000UL) == 0) {
        n = n + 16;
        x = x << 16;
    }
    if ((x & 0xFF000000UL) == 0) {
        n = n + 8;
        x = x << 8;
    }
    if ((x & 0xF0000000UL) == 0) {
        n = n + 4;
        x = x << 4;
    }
    if ((x & 0xC0000000UL) == 0) {
        n = n + 2;
        x = x << 2;
    }
    if ((x & 0x80000000UL) == 0) {
        n = n + 1;
    }
    return n;
}

static int clz64(unsigned long long x) {
    unsigned long hi;
    unsigned long lo;

    hi = (unsigned long)(x >> 32);
    lo = (unsigned long)x;
    if (hi != 0) {
        return clz32(hi);
    }
    return 32 + clz32(lo);
}

static int f32_isnan(unsigned long a) {
    return ((a & F32_EXP) == F32_EXP) && ((a & F32_FRAC) != 0);
}

static int f32_isinf(unsigned long a) {
    return ((a & F32_EXP) == F32_EXP) && ((a & F32_FRAC) == 0);
}

static int f32_iszero(unsigned long a) {
    return (a & ~F32_SIGN) == 0;
}

static unsigned long f32_pack(unsigned long sign, int exp, unsigned long frac) {
    unsigned long e;

    if (exp >= 255) {
        return sign | F32_INF;
    }
    if (exp <= 0) {
        if (exp < -23) {
            return sign;
        }
        frac = (frac | 0x800000UL) >> (1 - exp);
        e = 0;
        return sign | e | (frac & F32_FRAC);
    }
    e = ((unsigned long)exp) << 23;
    return sign | e | (frac & F32_FRAC);
}

unsigned long __rcc_f32_add(unsigned long a, unsigned long b) {
    unsigned long sign_a;
    unsigned long sign_b;
    int exp_a;
    int exp_b;
    unsigned long frac_a;
    unsigned long frac_b;
    int diff;
    unsigned long long sig_a;
    unsigned long long sig_b;
    unsigned long sign;
    int exp;
    unsigned long long sig;

    if (f32_isnan(a)) {
        return F32_QNAN;
    }
    if (f32_isnan(b)) {
        return F32_QNAN;
    }
    if (f32_isinf(a) && f32_isinf(b)) {
        if ((a ^ b) & F32_SIGN) {
            return F32_QNAN;
        }
        return a;
    }
    if (f32_isinf(a)) {
        return a;
    }
    if (f32_isinf(b)) {
        return b;
    }
    if (f32_iszero(a)) {
        if (f32_iszero(b)) {
            return a & b;
        }
        return b;
    }
    if (f32_iszero(b)) {
        return a;
    }

    sign_a = a & F32_SIGN;
    sign_b = b & F32_SIGN;
    exp_a = (int)((a & F32_EXP) >> 23);
    exp_b = (int)((b & F32_EXP) >> 23);
    frac_a = a & F32_FRAC;
    frac_b = b & F32_FRAC;

    if (exp_a == 0) {
        exp_a = 1;
    } else {
        frac_a = frac_a | 0x800000UL;
    }
    if (exp_b == 0) {
        exp_b = 1;
    } else {
        frac_b = frac_b | 0x800000UL;
    }

    sig_a = ((unsigned long long)frac_a) << 3;
    sig_b = ((unsigned long long)frac_b) << 3;

    if (exp_a < exp_b) {
        diff = exp_b - exp_a;
        if (diff > 31) {
            sig_a = 0;
        } else {
            sig_a = sig_a >> diff;
        }
        exp_a = exp_b;
    } else if (exp_b < exp_a) {
        diff = exp_a - exp_b;
        if (diff > 31) {
            sig_b = 0;
        } else {
            sig_b = sig_b >> diff;
        }
    }

    exp = exp_a;
    if (sign_a == sign_b) {
        sign = sign_a;
        sig = sig_a + sig_b;
        if (sig & (1ULL << 27)) {
            sig = sig >> 1;
            exp = exp + 1;
        }
        return f32_pack(sign, exp, (unsigned long)(sig >> 3));
    }

    if (sig_a >= sig_b) {
        sign = sign_a;
        sig = sig_a - sig_b;
    } else {
        sign = sign_b;
        sig = sig_b - sig_a;
    }
    if (sig == 0) {
        return 0;
    }
    while ((sig & (1ULL << 26)) == 0) {
        sig = sig << 1;
        exp = exp - 1;
        if (exp <= 0) {
            return sign;
        }
    }
    return f32_pack(sign, exp, (unsigned long)(sig >> 3));
}

unsigned long __rcc_f32_sub(unsigned long a, unsigned long b) {
    return __rcc_f32_add(a, b ^ F32_SIGN);
}

unsigned long __rcc_f32_mul(unsigned long a, unsigned long b) {
    unsigned long sign;
    int exp_a;
    int exp_b;
    unsigned long frac_a;
    unsigned long frac_b;
    int exp;
    unsigned long long prod;

    sign = (a ^ b) & F32_SIGN;
    if (f32_isnan(a) || f32_isnan(b)) {
        return F32_QNAN;
    }
    if ((f32_isinf(a) && f32_iszero(b)) || (f32_isinf(b) && f32_iszero(a))) {
        return F32_QNAN;
    }
    if (f32_isinf(a) || f32_isinf(b)) {
        return sign | F32_INF;
    }
    if (f32_iszero(a) || f32_iszero(b)) {
        return sign;
    }

    exp_a = (int)((a & F32_EXP) >> 23);
    exp_b = (int)((b & F32_EXP) >> 23);
    frac_a = a & F32_FRAC;
    frac_b = b & F32_FRAC;
    if (exp_a == 0) {
        exp_a = 1;
    } else {
        frac_a = frac_a | 0x800000UL;
    }
    if (exp_b == 0) {
        exp_b = 1;
    } else {
        frac_b = frac_b | 0x800000UL;
    }

    exp = exp_a + exp_b - 127;
    prod = (unsigned long long)frac_a * (unsigned long long)frac_b;
    /* 24 x 24 -> 48 bits. Hidden bit of the product sits at bit 46 or 47. */
    if (prod & (1ULL << 47)) {
        prod = prod >> 24;
        exp = exp + 1;
    } else {
        prod = prod >> 23;
    }
    return f32_pack(sign, exp, (unsigned long)prod);
}

unsigned long __rcc_f32_div(unsigned long a, unsigned long b) {
    unsigned long sign;
    int exp_a;
    int exp_b;
    unsigned long frac_a;
    unsigned long frac_b;
    int exp;
    unsigned long long num;
    unsigned long long den;
    unsigned long long q;

    sign = (a ^ b) & F32_SIGN;
    if (f32_isnan(a) || f32_isnan(b)) {
        return F32_QNAN;
    }
    if (f32_iszero(a) && f32_iszero(b)) {
        return F32_QNAN;
    }
    if (f32_isinf(a) && f32_isinf(b)) {
        return F32_QNAN;
    }
    if (f32_isinf(b) || f32_iszero(a)) {
        return sign;
    }
    if (f32_isinf(a) || f32_iszero(b)) {
        return sign | F32_INF;
    }

    exp_a = (int)((a & F32_EXP) >> 23);
    exp_b = (int)((b & F32_EXP) >> 23);
    frac_a = a & F32_FRAC;
    frac_b = b & F32_FRAC;
    if (exp_a == 0) {
        exp_a = 1;
    } else {
        frac_a = frac_a | 0x800000UL;
    }
    if (exp_b == 0) {
        exp_b = 1;
    } else {
        frac_b = frac_b | 0x800000UL;
    }

    exp = exp_a - exp_b + 127;
    num = ((unsigned long long)frac_a) << 23;
    den = (unsigned long long)frac_b;
    q = num / den;
    if ((q & 0x800000UL) == 0) {
        q = q << 1;
        exp = exp - 1;
    }
    return f32_pack(sign, exp, (unsigned long)q);
}

int __rcc_f32_eq(unsigned long a, unsigned long b) {
    if (f32_isnan(a) || f32_isnan(b)) {
        return 0;
    }
    if (f32_iszero(a) && f32_iszero(b)) {
        return 1;
    }
    return a == b;
}

int __rcc_f32_ne(unsigned long a, unsigned long b) {
    return !__rcc_f32_eq(a, b);
}

int __rcc_f32_lt(unsigned long a, unsigned long b) {
    unsigned long sa;
    unsigned long sb;

    if (f32_isnan(a) || f32_isnan(b)) {
        return 0;
    }
    if (f32_iszero(a) && f32_iszero(b)) {
        return 0;
    }
    sa = a & F32_SIGN;
    sb = b & F32_SIGN;
    if (sa != sb) {
        return sa != 0;
    }
    if (sa) {
        return a > b;
    }
    return a < b;
}

int __rcc_f32_le(unsigned long a, unsigned long b) {
    return __rcc_f32_lt(a, b) || __rcc_f32_eq(a, b);
}

int __rcc_f32_gt(unsigned long a, unsigned long b) {
    return __rcc_f32_lt(b, a);
}

int __rcc_f32_ge(unsigned long a, unsigned long b) {
    return __rcc_f32_le(b, a);
}

unsigned long __rcc_f32_from_si(long x) {
    unsigned long sign;
    unsigned long mag;
    int shift;
    int exp;

    if (x == 0) {
        return 0;
    }
    mag = (unsigned long)x;
    if (x < 0) {
        sign = F32_SIGN;
        mag = 0UL - mag;
    } else {
        sign = 0;
    }
    shift = clz32(mag);
    exp = 158 - shift; /* 127 + 31 - shift */
    if (shift >= 8) {
        mag = mag << (shift - 8);
    } else {
        mag = mag >> (8 - shift);
    }
    return f32_pack(sign, exp, mag);
}

unsigned long __rcc_f32_from_ui(unsigned long x) {
    int shift;
    int exp;

    if (x == 0) {
        return 0;
    }
    shift = clz32(x);
    exp = 158 - shift;
    if (shift >= 8) {
        x = x << (shift - 8);
    } else {
        x = x >> (8 - shift);
    }
    return f32_pack(0, exp, x);
}

unsigned long __rcc_f32_from_i64(long long x) {
    unsigned long sign;
    unsigned long long mag;
    int shift;
    int exp;
    unsigned long frac;

    if (x == 0) {
        return 0;
    }
    if (x < 0) {
        sign = F32_SIGN;
        mag = (unsigned long long)(0 - x);
    } else {
        sign = 0;
        mag = (unsigned long long)x;
    }
    shift = clz64(mag);
    exp = 190 - shift; /* 127 + 63 - shift */
    if (shift <= 40) {
        mag = mag >> (40 - shift);
    } else {
        mag = mag << (shift - 40);
    }
    frac = (unsigned long)mag;
    return f32_pack(sign, exp, frac);
}

unsigned long __rcc_f32_from_u64(unsigned long long x) {
    int shift;
    int exp;
    unsigned long frac;

    if (x == 0) {
        return 0;
    }
    shift = clz64(x);
    exp = 190 - shift;
    if (shift <= 40) {
        x = x >> (40 - shift);
    } else {
        x = x << (shift - 40);
    }
    frac = (unsigned long)x;
    return f32_pack(0, exp, frac);
}

long __rcc_f32_to_si(unsigned long a) {
    unsigned long sign;
    int exp;
    unsigned long frac;
    int shift;
    unsigned long mag;
    long r;

    if (f32_isnan(a)) {
        return 0;
    }
    sign = a & F32_SIGN;
    exp = (int)((a & F32_EXP) >> 23);
    frac = a & F32_FRAC;
    if (exp == 0) {
        return 0;
    }
    if (exp == 255) {
        if (sign) {
            return (long)0x80000000UL;
        }
        return 0x7FFFFFFF;
    }
    frac = frac | 0x800000UL;
    shift = exp - 150; /* 127 + 23 */
    if (shift >= 8) {
        if (sign) {
            return (long)0x80000000UL;
        }
        return 0x7FFFFFFF;
    }
    if (shift >= 0) {
        mag = frac << shift;
    } else {
        if (shift <= -24) {
            return 0;
        }
        mag = frac >> (-shift);
    }
    if (sign) {
        r = 0 - (long)mag;
        return r;
    }
    return (long)mag;
}

unsigned long __rcc_f32_to_ui(unsigned long a) {
    long v;

    v = __rcc_f32_to_si(a);
    if (v < 0) {
        return 0;
    }
    return (unsigned long)v;
}

long long __rcc_f32_to_i64(unsigned long a) {
    return (long long)__rcc_f32_to_si(a);
}

unsigned long long __rcc_f32_to_u64(unsigned long a) {
    return (unsigned long long)__rcc_f32_to_ui(a);
}

static int f64_isnan(unsigned long long a) {
    return ((a & F64_EXP) == F64_EXP) && ((a & F64_FRAC) != 0);
}

static int f64_isinf(unsigned long long a) {
    return ((a & F64_EXP) == F64_EXP) && ((a & F64_FRAC) == 0);
}

static int f64_iszero(unsigned long long a) {
    return (a & ~F64_SIGN) == 0;
}

/* Working significand: hidden bit at bit 62, 10 extra bits at [9:0] for rounding. */
static unsigned long long f64_shr_sticky(unsigned long long x, int n, int *sticky) {
    if (n <= 0) {
        return x;
    }
    if (n >= 64) {
        if (x != 0) {
            *sticky = 1;
        }
        return 0;
    }
    if ((x & ((1ULL << n) - 1ULL)) != 0) {
        *sticky = 1;
    }
    return x >> n;
}

static unsigned long long f64_round_pack(unsigned long long sign, int exp, unsigned long long sig, int sticky) {
    unsigned long long extra;
    int odd;

    if (sig & (1ULL << 63)) {
        sticky |= (int)(sig & 1ULL);
        sig = sig >> 1;
        exp = exp + 1;
    }

    if (exp <= 0) {
        sig = f64_shr_sticky(sig, 1 - exp, &sticky);
        exp = 0;
    }

    extra = sig & 0x3FFULL;
    odd = (int)((sig >> 10) & 1ULL);
    if (extra > 0x200ULL || (extra == 0x200ULL && (sticky || odd))) {
        sig = sig + 0x400ULL;
        if (sig & (1ULL << 63)) {
            sig = sig >> 1;
            exp = exp + 1;
        }
    }

    sig = sig >> 10;
    if (exp >= 2047) {
        return sign | F64_INF;
    }
    if (exp <= 0) {
        if (sig & (1ULL << 52)) {
            return sign | (1ULL << 52);
        }
        return sign | (sig & F64_FRAC);
    }
    if (sig & (1ULL << 53)) {
        sig = sig >> 1;
        exp = exp + 1;
        if (exp >= 2047) {
            return sign | F64_INF;
        }
    }
    return sign | (((unsigned long long)exp) << 52) | (sig & F64_FRAC);
}

static unsigned long long f64_pack(unsigned long long sign, int exp, unsigned long long frac) {
    unsigned long long e;

    if (exp >= 2047) {
        return sign | F64_INF;
    }
    if (exp <= 0) {
        return sign;
    }
    e = ((unsigned long long)exp) << 52;
    return sign | e | (frac & F64_FRAC);
}

static void mul_u64(unsigned long long a, unsigned long long b,
                    unsigned long long *hi, unsigned long long *lo) {
    unsigned long a0;
    unsigned long a1;
    unsigned long b0;
    unsigned long b1;
    unsigned long long p00;
    unsigned long long p01;
    unsigned long long p10;
    unsigned long long p11;
    unsigned long long mid;

    a0 = (unsigned long)a;
    a1 = (unsigned long)(a >> 32);
    b0 = (unsigned long)b;
    b1 = (unsigned long)(b >> 32);
    p00 = (unsigned long long)a0 * (unsigned long long)b0;
    p01 = (unsigned long long)a0 * (unsigned long long)b1;
    p10 = (unsigned long long)a1 * (unsigned long long)b0;
    p11 = (unsigned long long)a1 * (unsigned long long)b1;
    mid = (p00 >> 32) + (p01 & 0xFFFFFFFFULL) + (p10 & 0xFFFFFFFFULL);
    *lo = (p00 & 0xFFFFFFFFULL) | (mid << 32);
    *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

/* 128-bit / 64-bit using 32-bit limbs. I64 shift/div in a C loop needs more
 * pinned T/S registers than the CPU has (the compiler parks I64 div in A/Rv/X).
 * Requires n1 < d so the quotient fits in 64 bits. */
static unsigned long long div128_64(unsigned long long n1, unsigned long long n0,
                                    unsigned long long d, unsigned long long *rem) {
    unsigned long r1;
    unsigned long r0;
    unsigned long n1h;
    unsigned long n0h;
    unsigned long d1;
    unsigned long d0;
    unsigned long q1;
    unsigned long q0;
    int i;

    r1 = (unsigned long)(n1 >> 32);
    r0 = (unsigned long)n1;
    n1h = (unsigned long)(n0 >> 32);
    n0h = (unsigned long)n0;
    d1 = (unsigned long)(d >> 32);
    d0 = (unsigned long)d;
    q1 = 0;
    q0 = 0;

    for (i = 0; i < 64; i++) {
        unsigned long msb;
        unsigned long bit;
        unsigned long carry;
        unsigned long nr1;
        unsigned long nr0;
        unsigned long br;

        msb = r1 >> 31;
        bit = n1h >> 31;
        carry = n0h >> 31;
        n0h = n0h + n0h;
        n1h = (n1h + n1h) | carry;
        carry = r0 >> 31;
        nr0 = (r0 + r0) | bit;
        nr1 = (r1 + r1) | carry;
        carry = q0 >> 31;
        q0 = q0 + q0;
        q1 = (q1 + q1) | carry;
        if (msb || nr1 > d1 || (nr1 == d1 && nr0 >= d0)) {
            br = nr0 < d0;
            r0 = nr0 - d0;
            r1 = nr1 - d1 - br;
            q0 = q0 | 1UL;
        } else {
            r0 = nr0;
            r1 = nr1;
        }
    }
    *rem = ((unsigned long long)r1 << 32) | (unsigned long long)r0;
    return ((unsigned long long)q1 << 32) | (unsigned long long)q0;
}

static unsigned long long f64_normalize_round(unsigned long long sign, int exp, unsigned long long sig, int sticky) {
    if (sig == 0) {
        return sign;
    }
    while ((sig & (1ULL << 62)) == 0) {
        sig = sig << 1;
        exp = exp - 1;
        if (exp <= 0) {
            break;
        }
    }
    return f64_round_pack(sign, exp, sig, sticky);
}

unsigned long long __rcc_f64_from_f32(unsigned long a) {
    unsigned long sign;
    int exp;
    unsigned long frac;
    unsigned long long f64frac;

    if (f32_isnan(a)) {
        return F64_QNAN;
    }
    sign = (unsigned long long)(a & F32_SIGN) << 32;
    exp = (int)((a & F32_EXP) >> 23);
    frac = a & F32_FRAC;
    if (exp == 255) {
        return sign | F64_INF;
    }
    if (exp == 0) {
        unsigned long m;
        int s;

        if (frac == 0) {
            return sign;
        }
        m = frac;
        s = clz32(m) - 8;
        m = m << s;
        exp = 1 - s;
        frac = m & F32_FRAC;
    }
    exp = exp + (1023 - 127);
    f64frac = ((unsigned long long)frac) << 29;
    return f64_pack(sign, exp, f64frac);
}

unsigned long __rcc_f32_from_f64(unsigned long long a) {
    unsigned long sign;
    int exp;
    unsigned long long frac;
    unsigned long f;
    unsigned long long round_bits;
    int sticky;
    int odd;

    if (f64_isnan(a)) {
        return F32_QNAN;
    }
    sign = (unsigned long)((a & F64_SIGN) >> 32) & F32_SIGN;
    exp = (int)((a & F64_EXP) >> 52);
    frac = a & F64_FRAC;
    if (exp == 2047) {
        return sign | F32_INF;
    }
    if (exp == 0) {
        return sign;
    }
    exp = exp - (1023 - 127);
    /* 52-bit frac -> 23-bit: keep top 23, round using the rest. */
    round_bits = frac & 0x1FFFFFFFULL;
    f = (unsigned long)(frac >> 29);
    sticky = (round_bits & 0x0FFFFFFFULL) != 0;
    odd = (int)(f & 1);
    if (round_bits > 0x10000000ULL || (round_bits == 0x10000000ULL && (sticky || odd))) {
        f = f + 1;
        /* Fraction is 23 bits (no hidden bit). Overflow means 1.11..1 rounded
         * to the next binade: increment exponent and clear the fraction. */
        if (f & 0x800000UL) {
            f = 0;
            exp = exp + 1;
        }
    }
    return f32_pack(sign, exp, f);
}

unsigned long long __rcc_f64_add(unsigned long long a, unsigned long long b) {
    unsigned long long sign_a;
    unsigned long long sign_b;
    int exp_a;
    int exp_b;
    unsigned long long frac_a;
    unsigned long long frac_b;
    unsigned long long sig_a;
    unsigned long long sig_b;
    unsigned long long sig;
    unsigned long long sign;
    int exp;
    int diff;
    int sticky;

    if (f64_isnan(a)) {
        return F64_QNAN;
    }
    if (f64_isnan(b)) {
        return F64_QNAN;
    }
    if (f64_isinf(a) && f64_isinf(b)) {
        if ((a ^ b) & F64_SIGN) {
            return F64_QNAN;
        }
        return a;
    }
    if (f64_isinf(a)) {
        return a;
    }
    if (f64_isinf(b)) {
        return b;
    }
    if (f64_iszero(a)) {
        if (f64_iszero(b)) {
            return a & b;
        }
        return b;
    }
    if (f64_iszero(b)) {
        return a;
    }

    sign_a = a & F64_SIGN;
    sign_b = b & F64_SIGN;
    exp_a = (int)((a & F64_EXP) >> 52);
    exp_b = (int)((b & F64_EXP) >> 52);
    frac_a = a & F64_FRAC;
    frac_b = b & F64_FRAC;
    sticky = 0;

    if (exp_a == 0) {
        exp_a = 1;
        sig_a = frac_a << 10;
    } else {
        sig_a = (frac_a | (1ULL << 52)) << 10;
    }
    if (exp_b == 0) {
        exp_b = 1;
        sig_b = frac_b << 10;
    } else {
        sig_b = (frac_b | (1ULL << 52)) << 10;
    }

    if (exp_a < exp_b) {
        diff = exp_b - exp_a;
        sig_a = f64_shr_sticky(sig_a, diff, &sticky);
        exp_a = exp_b;
    } else if (exp_b < exp_a) {
        diff = exp_a - exp_b;
        sig_b = f64_shr_sticky(sig_b, diff, &sticky);
    }

    exp = exp_a;
    if (sign_a == sign_b) {
        sign = sign_a;
        sig = sig_a + sig_b;
        return f64_round_pack(sign, exp, sig, sticky);
    }

    if (sig_a > sig_b) {
        sign = sign_a;
        sig = sig_a - sig_b;
    } else if (sig_b > sig_a) {
        sign = sign_b;
        sig = sig_b - sig_a;
    } else {
        return 0;
    }
    return f64_normalize_round(sign, exp, sig, sticky);
}

unsigned long long __rcc_f64_sub(unsigned long long a, unsigned long long b) {
    return __rcc_f64_add(a, b ^ F64_SIGN);
}

unsigned long long __rcc_f64_mul(unsigned long long a, unsigned long long b) {
    unsigned long long sign;
    int exp_a;
    int exp_b;
    unsigned long long frac_a;
    unsigned long long frac_b;
    unsigned long long sig_a;
    unsigned long long sig_b;
    unsigned long long hi;
    unsigned long long lo;
    int exp;
    int sticky;
    unsigned long long sig;

    sign = (a ^ b) & F64_SIGN;
    if (f64_isnan(a) || f64_isnan(b)) {
        return F64_QNAN;
    }
    if ((f64_isinf(a) && f64_iszero(b)) || (f64_isinf(b) && f64_iszero(a))) {
        return F64_QNAN;
    }
    if (f64_isinf(a) || f64_isinf(b)) {
        return sign | F64_INF;
    }
    if (f64_iszero(a) || f64_iszero(b)) {
        return sign;
    }

    exp_a = (int)((a & F64_EXP) >> 52);
    exp_b = (int)((b & F64_EXP) >> 52);
    frac_a = a & F64_FRAC;
    frac_b = b & F64_FRAC;
    if (exp_a == 0) {
        sig_a = frac_a;
        exp_a = 1;
        while ((sig_a & (1ULL << 52)) == 0) {
            sig_a = sig_a << 1;
            exp_a = exp_a - 1;
        }
    } else {
        sig_a = frac_a | (1ULL << 52);
    }
    if (exp_b == 0) {
        sig_b = frac_b;
        exp_b = 1;
        while ((sig_b & (1ULL << 52)) == 0) {
            sig_b = sig_b << 1;
            exp_b = exp_b - 1;
        }
    } else {
        sig_b = frac_b | (1ULL << 52);
    }

    exp = exp_a + exp_b - 1023;
    mul_u64(sig_a, sig_b, &hi, &lo);
    sticky = 0;
    /* 53x53 product is in [2^104, 2^106). Leading 1 at bit 104 or 105. */
    if (hi & (1ULL << 41)) {
        /* bit 105 set: take bits 105..42 as 64-bit working sig (hidden at 63, then we shift) */
        sticky = (lo & ((1ULL << 42) - 1ULL)) != 0;
        sig = (hi << 22) | (lo >> 42);
        exp = exp + 1;
    } else {
        sticky = (lo & ((1ULL << 41) - 1ULL)) != 0;
        sig = (hi << 23) | (lo >> 41);
    }
    /* sig now has hidden bit at 63. round_pack expects it at 62. */
    sticky |= (int)(sig & 1ULL);
    sig = sig >> 1;
    return f64_round_pack(sign, exp, sig, sticky);
}

unsigned long long __rcc_f64_div(unsigned long long a, unsigned long long b) {
    unsigned long long sign;
    int exp_a;
    int exp_b;
    unsigned long long frac_a;
    unsigned long long frac_b;
    unsigned long long sig_a;
    unsigned long long sig_b;
    int exp;
    unsigned long long q;
    unsigned long long rem;
    int sticky;
    unsigned long long sig;

    sign = (a ^ b) & F64_SIGN;
    if (f64_isnan(a) || f64_isnan(b)) {
        return F64_QNAN;
    }
    if (f64_iszero(a) && f64_iszero(b)) {
        return F64_QNAN;
    }
    if (f64_isinf(a) && f64_isinf(b)) {
        return F64_QNAN;
    }
    if (f64_isinf(b) || f64_iszero(a)) {
        return sign;
    }
    if (f64_isinf(a) || f64_iszero(b)) {
        return sign | F64_INF;
    }

    exp_a = (int)((a & F64_EXP) >> 52);
    exp_b = (int)((b & F64_EXP) >> 52);
    frac_a = a & F64_FRAC;
    frac_b = b & F64_FRAC;
    if (exp_a == 0) {
        sig_a = frac_a;
        exp_a = 1;
        while ((sig_a & (1ULL << 52)) == 0) {
            sig_a = sig_a << 1;
            exp_a = exp_a - 1;
        }
    } else {
        sig_a = frac_a | (1ULL << 52);
    }
    if (exp_b == 0) {
        sig_b = frac_b;
        exp_b = 1;
        while ((sig_b & (1ULL << 52)) == 0) {
            sig_b = sig_b << 1;
            exp_b = exp_b - 1;
        }
    } else {
        sig_b = frac_b | (1ULL << 52);
    }

    exp = exp_a - exp_b + 1023;
    /* q = (sig_a << 61) / sig_b  in ~[2^60, 2^62) */
    q = div128_64(sig_a >> 3, sig_a << 61, sig_b, &rem);
    sticky = rem != 0;
    if (q < (1ULL << 61)) {
        sig = q << 2;
        exp = exp - 1;
    } else {
        sig = q << 1;
    }
    return f64_round_pack(sign, exp, sig, sticky);
}

int __rcc_f64_eq(unsigned long long a, unsigned long long b) {
    if (f64_isnan(a) || f64_isnan(b)) {
        return 0;
    }
    if (f64_iszero(a) && f64_iszero(b)) {
        return 1;
    }
    return a == b;
}

int __rcc_f64_ne(unsigned long long a, unsigned long long b) {
    return !__rcc_f64_eq(a, b);
}

int __rcc_f64_lt(unsigned long long a, unsigned long long b) {
    unsigned long long sa;
    unsigned long long sb;

    if (f64_isnan(a) || f64_isnan(b)) {
        return 0;
    }
    if (f64_iszero(a) && f64_iszero(b)) {
        return 0;
    }
    sa = a & F64_SIGN;
    sb = b & F64_SIGN;
    if (sa != sb) {
        return sa != 0;
    }
    if (sa) {
        return a > b;
    }
    return a < b;
}

int __rcc_f64_le(unsigned long long a, unsigned long long b) {
    return __rcc_f64_lt(a, b) || __rcc_f64_eq(a, b);
}

int __rcc_f64_gt(unsigned long long a, unsigned long long b) {
    return __rcc_f64_lt(b, a);
}

int __rcc_f64_ge(unsigned long long a, unsigned long long b) {
    return __rcc_f64_le(b, a);
}

unsigned long long __rcc_f64_from_u64(unsigned long long x) {
    int shift;
    int exp;
    unsigned long long sig;
    int sticky;

    if (x == 0) {
        return 0;
    }
    shift = clz64(x);
    exp = 1023 + (63 - shift);
    sticky = 0;
    if (shift == 0) {
        sticky = (int)(x & 1ULL);
        sig = x >> 1;
    } else {
        sig = x << (shift - 1);
    }
    return f64_round_pack(0, exp, sig, sticky);
}

unsigned long long __rcc_f64_from_i64(long long x) {
    unsigned long long sign;
    unsigned long long mag;

    if (x == 0) {
        return 0;
    }
    if (x < 0) {
        sign = F64_SIGN;
        mag = 0ULL - (unsigned long long)x;
    } else {
        sign = 0;
        mag = (unsigned long long)x;
    }
    return sign | __rcc_f64_from_u64(mag);
}

unsigned long long __rcc_f64_from_si(long x) {
    return __rcc_f64_from_i64((long long)x);
}

unsigned long long __rcc_f64_from_ui(unsigned long x) {
    return __rcc_f64_from_u64((unsigned long long)x);
}

long long __rcc_f64_to_i64(unsigned long long a) {
    unsigned long long sign;
    int exp;
    unsigned long long mag;
    int shift;

    if (f64_isnan(a)) {
        return 0;
    }
    sign = a & F64_SIGN;
    exp = (int)((a & F64_EXP) >> 52);
    mag = a & F64_FRAC;
    if (exp == 0) {
        return 0;
    }
    if (exp == 2047) {
        if (sign) {
            return (long long)0x8000000000000000ULL;
        }
        return 0x7FFFFFFFFFFFFFFFLL;
    }
    mag = mag | (1ULL << 52);
    shift = exp - 1075; /* exp - 1023 - 52 */
    /* mag << 11 is in [2^63, 2^64): does not fit in signed 64-bit. */
    if (shift >= 11) {
        if (sign) {
            return (long long)0x8000000000000000ULL;
        }
        return 0x7FFFFFFFFFFFFFFFLL;
    }
    if (shift >= 0) {
        mag = mag << shift;
    } else {
        if (shift <= -53) {
            return 0;
        }
        mag = mag >> (-shift);
    }
    if (sign) {
        return (long long)(0ULL - mag);
    }
    return (long long)mag;
}

long __rcc_f64_to_si(unsigned long long a) {
    long long v;

    v = __rcc_f64_to_i64(a);
    if (v > 0x7FFFFFFFLL) {
        return 0x7FFFFFFF;
    }
    if (v < -2147483647LL - 1) {
        return (long)0x80000000UL;
    }
    return (long)v;
}

unsigned long __rcc_f64_to_ui(unsigned long long a) {
    long long v;

    v = __rcc_f64_to_i64(a);
    if (v < 0) {
        return 0;
    }
    if (v > 0xFFFFFFFFLL) {
        return 0xFFFFFFFFUL;
    }
    return (unsigned long)v;
}

unsigned long long __rcc_f64_to_u64(unsigned long long a) {
    int exp;
    unsigned long long mag;
    int shift;

    if (f64_isnan(a) || (a & F64_SIGN)) {
        return 0;
    }
    exp = (int)((a & F64_EXP) >> 52);
    mag = a & F64_FRAC;
    if (exp == 0) {
        return 0;
    }
    if (exp == 2047) {
        return 0xFFFFFFFFFFFFFFFFULL;
    }
    mag = mag | (1ULL << 52);
    shift = exp - 1075;
    if (shift >= 12) {
        return 0xFFFFFFFFFFFFFFFFULL;
    }
    if (shift >= 0) {
        mag = mag << shift;
    } else {
        if (shift <= -53) {
            return 0;
        }
        mag = mag >> (-shift);
    }
    return mag;
}
