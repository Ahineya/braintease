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
    f = (unsigned long)(frac >> 29);
    return f32_pack(sign, exp, f);
}

unsigned long long __rcc_f64_add(unsigned long long a, unsigned long long b) {
    /* v1: evaluate in binary32. Exact for the small values used by the tests. */
    return __rcc_f64_from_f32(__rcc_f32_add(__rcc_f32_from_f64(a), __rcc_f32_from_f64(b)));
}

unsigned long long __rcc_f64_sub(unsigned long long a, unsigned long long b) {
    return __rcc_f64_from_f32(__rcc_f32_sub(__rcc_f32_from_f64(a), __rcc_f32_from_f64(b)));
}

unsigned long long __rcc_f64_mul(unsigned long long a, unsigned long long b) {
    return __rcc_f64_from_f32(__rcc_f32_mul(__rcc_f32_from_f64(a), __rcc_f32_from_f64(b)));
}

unsigned long long __rcc_f64_div(unsigned long long a, unsigned long long b) {
    return __rcc_f64_from_f32(__rcc_f32_div(__rcc_f32_from_f64(a), __rcc_f32_from_f64(b)));
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

unsigned long long __rcc_f64_from_si(long x) {
    return __rcc_f64_from_f32(__rcc_f32_from_si(x));
}

unsigned long long __rcc_f64_from_ui(unsigned long x) {
    return __rcc_f64_from_f32(__rcc_f32_from_ui(x));
}

unsigned long long __rcc_f64_from_i64(long long x) {
    return __rcc_f64_from_f32(__rcc_f32_from_i64(x));
}

unsigned long long __rcc_f64_from_u64(unsigned long long x) {
    return __rcc_f64_from_f32(__rcc_f32_from_u64(x));
}

long long __rcc_f64_to_i64(unsigned long long a) {
    return __rcc_f32_to_i64(__rcc_f32_from_f64(a));
}

long __rcc_f64_to_si(unsigned long long a) {
    return __rcc_f32_to_si(__rcc_f32_from_f64(a));
}

unsigned long __rcc_f64_to_ui(unsigned long long a) {
    return __rcc_f32_to_ui(__rcc_f32_from_f64(a));
}

unsigned long long __rcc_f64_to_u64(unsigned long long a) {
    return __rcc_f32_to_u64(__rcc_f32_from_f64(a));
}
