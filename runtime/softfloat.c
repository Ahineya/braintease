#include "include/__float_accel.h"
/* IEEE-754 binary32 / binary64 for Ripple, with optional host acceleration.
 * Bit patterns are passed as unsigned long (f32) and unsigned long long (f64).
 * The fallback uses integer arithmetic only. */

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

/* A 27-bit significand fits in two VM words: 24 value bits plus guard,
 * round and sticky. Jam discarded bits into bit zero before each rounding. */
static unsigned long f32_shr_jam(unsigned long x, int n) {
    if (n <= 0) { return x; }
    if (n >= 32) { return x != 0; }
    return (x >> n) | ((x & ((1UL << n) - 1UL)) != 0);
}

static unsigned long f32_round_pack(unsigned long sign, int exp, unsigned long sig) {
    unsigned long extra;
    if (sig & (1UL << 27)) {
        sig = f32_shr_jam(sig, 1);
        exp = exp + 1;
    }
    if (exp <= 0) {
        sig = f32_shr_jam(sig, 1 - exp);
        exp = 0;
    }
    extra = sig & 7UL;
    sig = sig >> 3;
    if (extra > 4UL || (extra == 4UL && (sig & 1UL))) {
        sig = sig + 1UL;
    }
    if (sig & (1UL << 24)) {
        sig = sig >> 1;
        exp = exp + 1;
    }
    if (exp == 1 && sig < (1UL << 23)) { exp = 0; }
    if (exp == 0 && (sig & (1UL << 23))) {
        exp = 1;
    }
    if (exp >= 255) { return sign | F32_INF; }
    return sign | ((unsigned long)exp << 23) | (sig & F32_FRAC);
}

unsigned long __rcc_f32_add(unsigned long a, unsigned long b) {
    RCC_ACCEL_RETURN(VM_MATH_F32_ADD, a, b);
    unsigned long sign_a;
    unsigned long sign_b;
    unsigned long sig_a;
    unsigned long sig_b;
    unsigned long sig;
    unsigned long sign;
    int exp_a;
    int exp_b;
    int exp;

    if (f32_isnan(a) || f32_isnan(b)) { return F32_QNAN; }
    if (f32_isinf(a)) {
        if (f32_isinf(b) && ((a ^ b) & F32_SIGN)) { return F32_QNAN; }
        return a;
    }
    if (f32_isinf(b)) { return b; }
    if (f32_iszero(a)) {
        if (f32_iszero(b)) { return a & b; }
        return b;
    }
    if (f32_iszero(b)) { return a; }
    sign_a = a & F32_SIGN;
    sign_b = b & F32_SIGN;
    exp_a = (int)((a & F32_EXP) >> 23);
    exp_b = (int)((b & F32_EXP) >> 23);
    sig_a = a & F32_FRAC;
    sig_b = b & F32_FRAC;
    if (exp_a == 0) { exp_a = 1; } else { sig_a = sig_a | 0x800000UL; }
    if (exp_b == 0) { exp_b = 1; } else { sig_b = sig_b | 0x800000UL; }
    sig_a = sig_a << 3;
    sig_b = sig_b << 3;
    if (exp_a < exp_b) {
        sig_a = f32_shr_jam(sig_a, exp_b - exp_a);
        exp_a = exp_b;
    } else if (exp_b < exp_a) {
        sig_b = f32_shr_jam(sig_b, exp_a - exp_b);
    }
    exp = exp_a;
    if (sign_a == sign_b) {
        return f32_round_pack(sign_a, exp, sig_a + sig_b);
    }
    if (sig_a >= sig_b) { sign = sign_a; sig = sig_a - sig_b; }
    else { sign = sign_b; sig = sig_b - sig_a; }
    if (sig == 0) { return 0; }
    while ((sig & (1UL << 26)) == 0 && exp > 1) {
        sig = sig << 1;
        exp = exp - 1;
    }
    if (exp == 1 && (sig & (1UL << 26)) == 0) {
        sig = sig << 1;
        exp = 0;
    }
    return f32_round_pack(sign, exp, sig);
}

unsigned long __rcc_f32_sub(unsigned long a, unsigned long b) {
    RCC_ACCEL_RETURN(VM_MATH_F32_SUB, a, b);
    return __rcc_f32_add(a, b ^ F32_SIGN);
}

unsigned long __rcc_f32_mul(unsigned long a, unsigned long b) {
    RCC_ACCEL_RETURN(VM_MATH_F32_MUL, a, b);
    unsigned long sign;
    unsigned long sig_a;
    unsigned long sig_b;
    unsigned long low;
    unsigned long cross;
    unsigned long high;
    unsigned long old_low;
    unsigned long sig;
    int exp_a;
    int exp_b;
    int exp;
    int shift;

    sign = (a ^ b) & F32_SIGN;
    if (f32_isnan(a) || f32_isnan(b)) { return F32_QNAN; }
    if ((f32_isinf(a) && f32_iszero(b)) || (f32_isinf(b) && f32_iszero(a))) { return F32_QNAN; }
    if (f32_isinf(a) || f32_isinf(b)) { return sign | F32_INF; }
    if (f32_iszero(a) || f32_iszero(b)) { return sign; }
    exp_a = (int)((a & F32_EXP) >> 23);
    exp_b = (int)((b & F32_EXP) >> 23);
    sig_a = a & F32_FRAC;
    sig_b = b & F32_FRAC;
    if (exp_a == 0) {
        shift = clz32(sig_a) - 8;
        sig_a = sig_a << shift;
        exp_a = 1 - shift;
    } else { sig_a = sig_a | 0x800000UL; }
    if (exp_b == 0) {
        shift = clz32(sig_b) - 8;
        sig_b = sig_b << shift;
        exp_b = 1 - shift;
    } else { sig_b = sig_b | 0x800000UL; }
    exp = exp_a + exp_b - 127;
    if (sig_a == 0x800000UL) { return f32_round_pack(sign, exp, sig_b << 3); }
    if (sig_b == 0x800000UL) { return f32_round_pack(sign, exp, sig_a << 3); }
    /* 24x24 -> 48 using 16-bit limbs, without a general 64-bit product. */
    low = (sig_a & 0xFFFFUL) * (sig_b & 0xFFFFUL);
    cross = (sig_a >> 16) * (sig_b & 0xFFFFUL) + (sig_a & 0xFFFFUL) * (sig_b >> 16);
    old_low = low;
    low = low + (cross << 16);
    high = (sig_a >> 16) * (sig_b >> 16) + (cross >> 16) + (low < old_low);
    shift = 20;
    if (high & 0x8000UL) { shift = 21; exp = exp + 1; }
    sig = (high << (32 - shift)) | (low >> shift);
    sig = sig | ((low & ((1UL << shift) - 1UL)) != 0);
    return f32_round_pack(sign, exp, sig);
}

unsigned long __rcc_f32_div(unsigned long a, unsigned long b) {
    RCC_ACCEL_RETURN(VM_MATH_F32_DIV, a, b);
    unsigned long sign;
    unsigned long sig_a;
    unsigned long sig_b;
    unsigned long q;
    unsigned long rem;
    int exp_a;
    int exp_b;
    int exp;
    int shift;
    int i;

    sign = (a ^ b) & F32_SIGN;
    if (f32_isnan(a) || f32_isnan(b)) { return F32_QNAN; }
    if ((f32_iszero(a) && f32_iszero(b)) || (f32_isinf(a) && f32_isinf(b))) { return F32_QNAN; }
    if (f32_isinf(b) || f32_iszero(a)) { return sign; }
    if (f32_isinf(a) || f32_iszero(b)) { return sign | F32_INF; }
    exp_a = (int)((a & F32_EXP) >> 23);
    exp_b = (int)((b & F32_EXP) >> 23);
    sig_a = a & F32_FRAC;
    sig_b = b & F32_FRAC;
    if (exp_a == 0) {
        shift = clz32(sig_a) - 8;
        sig_a = sig_a << shift;
        exp_a = 1 - shift;
    } else { sig_a = sig_a | 0x800000UL; }
    if (exp_b == 0) {
        shift = clz32(sig_b) - 8;
        sig_b = sig_b << shift;
        exp_b = 1 - shift;
    } else { sig_b = sig_b | 0x800000UL; }
    exp = exp_a - exp_b + 127;
    if (sig_b == 0x800000UL) { return f32_round_pack(sign, exp, sig_a << 3); }
    if (sig_a < sig_b) { sig_a = sig_a << 1; exp = exp - 1; }
    rem = sig_a - sig_b;
    q = 1;
    for (i = 0; i < 25; i++) {
        rem = rem + rem;
        q = q + q;
        if (rem >= sig_b) { rem = rem - sig_b; q = q | 1UL; }
    }
    return f32_round_pack(sign, exp, (q << 1) | (rem != 0));
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

unsigned long __rcc_f32_from_ui(unsigned long x) {
    int shift;
    int exp;
    unsigned long extra;
    if (x == 0) { return 0; }
    shift = clz32(x);
    exp = 158 - shift;
    if (shift >= 5) { x = x << (shift - 5); }
    else { x = f32_shr_jam(x, 5 - shift); }
    /* Integer inputs are normalized and cannot underflow or overflow binary32.
     * Adding the hidden bit to (exp-1) also handles a rounding carry. */
    extra = x & 7UL;
    x = x >> 3;
    if (extra > 4UL || (extra == 4UL && (x & 1UL))) { x = x + 1UL; }
    return ((unsigned long)(exp - 1) << 23) + x;
}

unsigned long __rcc_f32_from_si(long x) {
    unsigned long mag;
    mag = (unsigned long)x;
    if (x < 0) { return F32_SIGN | __rcc_f32_from_ui(0UL - mag); }
    return __rcc_f32_from_ui(mag);
}

unsigned long __rcc_f32_from_u64(unsigned long long x) {
    RCC_ACCEL_RETURN(VM_MATH_F32_FROM_U64, x, 0);
    int shift;
    int exp;
    unsigned long extra;
    unsigned long sig;
    int sticky;
    if (x == 0) { return 0; }
    shift = clz64(x);
    exp = 190 - shift;
    if (shift >= 37) { sig = (unsigned long)x << (shift - 37); }
    else {
        shift = 37 - shift;
        sticky = (x & ((1ULL << shift) - 1ULL)) != 0;
        sig = (unsigned long)(x >> shift) | sticky;
    }
    /* Integer inputs are normalized and cannot underflow or overflow binary32.
     * Adding the hidden bit to (exp-1) also handles a rounding carry. */
    extra = sig & 7UL;
    sig = sig >> 3;
    if (extra > 4UL || (extra == 4UL && (sig & 1UL))) { sig = sig + 1UL; }
    return ((unsigned long)(exp - 1) << 23) + sig;
}

unsigned long __rcc_f32_from_i64(long long x) {
    RCC_ACCEL_RETURN(VM_MATH_F32_FROM_I64, x, 0);
    unsigned long long mag;
    mag = (unsigned long long)x;
    if (x < 0) { return F32_SIGN | __rcc_f32_from_u64(0ULL - mag); }
    return __rcc_f32_from_u64(mag);
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
    int exp;
    unsigned long sig;
    if ((a & F32_SIGN) || f32_isnan(a)) { return 0; }
    exp = (int)((a & F32_EXP) >> 23);
    if (exp >= 159) { return 0xFFFFFFFFUL; }
    if (exp < 127) { return 0; }
    sig = (a & F32_FRAC) | 0x800000UL;
    if (exp >= 150) { return sig << (exp - 150); }
    return sig >> (150 - exp);
}

unsigned long long __rcc_f32_to_u64(unsigned long a) {
    RCC_ACCEL_RETURN(VM_MATH_F32_TO_U64, a, 0);
    int exp;
    unsigned long long sig;
    if ((a & F32_SIGN) || f32_isnan(a)) { return 0; }
    exp = (int)((a & F32_EXP) >> 23);
    if (exp >= 191) { return 0xFFFFFFFFFFFFFFFFULL; }
    if (exp < 127) { return 0; }
    sig = (unsigned long long)((a & F32_FRAC) | 0x800000UL);
    if (exp >= 150) { return sig << (exp - 150); }
    return sig >> (150 - exp);
}

long long __rcc_f32_to_i64(unsigned long a) {
    RCC_ACCEL_RETURN(VM_MATH_F32_TO_I64, a, 0);
    int exp;
    unsigned long long mag;
    if (f32_isnan(a)) { return 0; }
    exp = (int)((a & F32_EXP) >> 23);
    if (exp >= 190) {
        if (a & F32_SIGN) { return (long long)0x8000000000000000ULL; }
        return 0x7FFFFFFFFFFFFFFFLL;
    }
    mag = __rcc_f32_to_u64(a & ~F32_SIGN);
    if (a & F32_SIGN) { return (long long)(0ULL - mag); }
    return (long long)mag;
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

/* A widening 32x32 product expressed using the VM's 16-bit word size.
 * Casting both inputs to u64 first asks the backend for a general 64x64 product,
 * including products of the zero upper halves. */
static unsigned long long mul_u32_wide(unsigned long a, unsigned long b) {
    unsigned long p00;
    unsigned long p01;
    unsigned long p10;
    unsigned long p11;
    unsigned long mid;
    unsigned long hi;
    unsigned long lo;
    p00 = (a & 65535UL) * (b & 65535UL);
    p01 = (a & 65535UL) * (b >> 16);
    p10 = (a >> 16) * (b & 65535UL);
    p11 = (a >> 16) * (b >> 16);
    mid = (p00 >> 16) + (p01 & 65535UL) + (p10 & 65535UL);
    lo = (p00 & 65535UL) | (mid << 16);
    hi = p11 + (p01 >> 16) + (p10 >> 16) + (mid >> 16);
    return ((unsigned long long)hi << 32) | (unsigned long long)lo;
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
    p00 = mul_u32_wide(a0, b0);
    p01 = mul_u32_wide(a0, b1);
    p10 = mul_u32_wide(a1, b0);
    p11 = mul_u32_wide(a1, b1);
    mid = (p00 >> 32) + (p01 & 0xFFFFFFFFULL) + (p10 & 0xFFFFFFFFULL);
    *lo = (p00 & 0xFFFFFFFFULL) | (mid << 32);
    *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

/* Divide normalized significands in [1, 2). The caller normalizes the ratio.
 * Keep 53 result bits, guard and round, then summarize the remainder as sticky.
 * The remainder is always below the 53-bit divisor, so doubling cannot overflow.
 * 32-bit limbs avoid a wide division and keep the loop small on the 16-bit VM. */
static unsigned long long f64_div_sig(unsigned long long a, unsigned long long b,
                                       int *sticky) {
    unsigned long r1;
    unsigned long r0;
    unsigned long d1;
    unsigned long d0;
    unsigned long q1;
    unsigned long q0;
    unsigned long long rem;
    int i;

    rem = a - b;
    r1 = (unsigned long)(rem >> 32);
    r0 = (unsigned long)rem;
    d1 = (unsigned long)(b >> 32);
    d0 = (unsigned long)b;
    q1 = 0;
    q0 = 1;
    for (i = 0; i < 54; i++) {
        unsigned long borrow;
        r1 = (r1 + r1) | (r0 >> 31);
        r0 = r0 + r0;
        q1 = (q1 + q1) | (q0 >> 31);
        q0 = q0 + q0;
        if (r1 > d1 || (r1 == d1 && r0 >= d0)) {
            borrow = r0 < d0;
            r0 = r0 - d0;
            r1 = r1 - d1 - borrow;
            q0 = q0 | 1UL;
        }
    }
    *sticky = r1 != 0 || r0 != 0;
    return (((unsigned long long)q1 << 32) | (unsigned long long)q0) << 8;
}

static unsigned long long f64_normalize_round(unsigned long long sign, int exp, unsigned long long sig, int sticky) {
    if (sig == 0) {
        return sign;
    }
    while ((sig & ((1ULL << 62) | (1ULL << 63))) == 0 && exp > 1) {
        sig = sig << 1;
        exp = exp - 1;
    }
    if (exp == 1 && (sig & ((1ULL << 62) | (1ULL << 63))) == 0) {
        /* Already scaled as a subnormal: compensate for round_pack's shift. */
        sig = sig << 1;
        exp = 0;
    }
    return f64_round_pack(sign, exp, sig, sticky);
}

unsigned long long __rcc_f64_from_f32(unsigned long a) {
    unsigned long long sign;
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
    unsigned long sig;
    if (f64_isnan(a)) { return F32_QNAN; }
    sign = (unsigned long)(a >> 32) & F32_SIGN;
    exp = (int)((a & F64_EXP) >> 52);
    if (exp == 2047) { return sign | F32_INF; }
    /* Every binary64 subnormal is below half the smallest binary32 value. */
    if (exp == 0) { return sign; }
    frac = (a & F64_FRAC) | (1ULL << 52);
    sig = (unsigned long)(frac >> 26) | ((frac & ((1ULL << 26) - 1ULL)) != 0);
    return f32_round_pack(sign, exp - (1023 - 127), sig);
}

unsigned long long __rcc_f64_add(unsigned long long a, unsigned long long b) {
    RCC_ACCEL_RETURN(VM_MATH_F64_ADD, a, b);
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
        if (exp == 1 && sig < (1ULL << 62)) {
            sig = sig << 1;
            exp = 0;
        }
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
    /* Alignment discarded a fraction of the smaller magnitude: subtract it,
     * rather than treating it as a positive tail of the difference. */
    if (sticky) {
        sig = sig - 1ULL;
    }
    return f64_normalize_round(sign, exp, sig, sticky);
}

unsigned long long __rcc_f64_sub(unsigned long long a, unsigned long long b) {
    RCC_ACCEL_RETURN(VM_MATH_F64_SUB, a, b);
    return __rcc_f64_add(a, b ^ F64_SIGN);
}

unsigned long long __rcc_f64_mul(unsigned long long a, unsigned long long b) {
    RCC_ACCEL_RETURN(VM_MATH_F64_MUL, a, b);
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
    /* Exact binary scaling needs no 106-bit product. Keep round_pack for
     * gradual underflow, exponent overflow and ties at the subnormal boundary. */
    if (sig_b == (1ULL << 52)) {
        return f64_round_pack(sign, exp, sig_a << 10, 0);
    }
    if (sig_a == (1ULL << 52)) {
        return f64_round_pack(sign, exp, sig_b << 10, 0);
    }
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
    RCC_ACCEL_RETURN(VM_MATH_F64_DIV, a, b);
    unsigned long long sign;
    int exp_a;
    int exp_b;
    unsigned long long frac_a;
    unsigned long long frac_b;
    unsigned long long sig_a;
    unsigned long long sig_b;
    int exp;
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
    if (sig_b == (1ULL << 52)) {
        return f64_round_pack(sign, exp, sig_a << 10, 0);
    }
    if (sig_a < sig_b) {
        sig_a = sig_a << 1;
        exp = exp - 1;
    }
    sig = f64_div_sig(sig_a, sig_b, &sticky);
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
    RCC_ACCEL_RETURN(VM_MATH_F64_FROM_U64, x, 0);
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
    RCC_ACCEL_RETURN(VM_MATH_F64_FROM_I64, x, 0);
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

unsigned long long __rcc_f64_from_ui(unsigned long x) {
    int shift;
    int exp;
    unsigned long long sig;
    if (x == 0) { return 0; }
    shift = clz32(x);
    exp = 1054 - shift;
    /* Every 32-bit integer fits exactly: no wide normalization or rounding. */
    sig = (unsigned long long)x << (shift + 21);
    return ((unsigned long long)exp << 52) | (sig & F64_FRAC);
}

unsigned long long __rcc_f64_from_si(long x) {
    if (x < 0) { return F64_SIGN | __rcc_f64_from_ui(0UL - (unsigned long)x); }
    return __rcc_f64_from_ui((unsigned long)x);
}

long long __rcc_f64_to_i64(unsigned long long a) {
    RCC_ACCEL_RETURN(VM_MATH_F64_TO_I64, a, 0);
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
    RCC_ACCEL_RETURN(VM_MATH_F64_TO_U64, a, 0);
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
