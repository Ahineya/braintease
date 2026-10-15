/* Integer-only binary64 helpers shared by RFL's math module.
 * No new guest instructions or host floating-point shortcuts. */
#define SIGN 0x8000000000000000ULL
#define FRAC 0x000FFFFFFFFFFFFFULL
#define INF  0x7FF0000000000000ULL
#define QNAN 0x7FF8000000000000ULL

extern unsigned long long __rcc_f64_from_u64(unsigned long long);

unsigned long long __rcc_f64_floor(unsigned long long a) {
    int exp;
    unsigned long long mask;
    exp = (int)((a >> 52) & 2047ULL);
    if (exp >= 1075) { return a; }
    if (exp < 1023) {
        if ((a & ~SIGN) == 0) { return a; }
        if (a & SIGN) { return 0xBFF0000000000000ULL; }
        return 0;
    }
    mask = (1ULL << (1075 - exp)) - 1ULL;
    if ((a & mask) == 0) { return a; }
    if (a & SIGN) { a = a + mask + 1ULL; }
    return a & ~mask;
}

unsigned long long __rcc_f64_sqrt(unsigned long long a) {
    unsigned long long sig;
    unsigned long long hi;
    unsigned long long lo;
    unsigned long long rem;
    unsigned long long root;
    unsigned long long trial;
    unsigned long long extra;
    int exp;
    int i;
    if ((a & ~SIGN) == 0 || a == INF) { return a; }
    if ((a & SIGN) || (a & INF) == INF) { return QNAN; }
    exp = (int)((a >> 52) & 2047ULL);
    sig = a & FRAC;
    if (exp == 0) {
        exp = 1;
        while ((sig & (1ULL << 52)) == 0) { sig = sig << 1; exp = exp - 1; }
    } else { sig = sig | (1ULL << 52); }
    exp = exp - 1023;
    if (exp & 1) { sig = sig << 1; exp = exp - 1; }
    exp = exp / 2 + 1023;
    /* sqrt(sig << 56) supplies 53 value bits plus guard and round. */
    hi = sig >> 8;
    lo = sig << 56;
    rem = 0;
    root = 0;
    for (i = 0; i < 55; i++) {
        rem = (rem << 2) | ((hi >> 44) & 3ULL);
        hi = (hi << 2) | (lo >> 62);
        lo = lo << 2;
        trial = (root << 2) | 1ULL;
        root = root << 1;
        if (rem >= trial) { rem = rem - trial; root = root | 1ULL; }
    }
    extra = root & 3ULL;
    root = root >> 2;
    if (extra > 2ULL || (extra == 2ULL && ((root & 1ULL) || rem != 0))) { root = root + 1ULL; }
    if (root & (1ULL << 53)) { root = root >> 1; exp = exp + 1; }
    return ((unsigned long long)exp << 52) | (root & FRAC);
}

/* floor((2/pi) * 2^1152), least significant 16-bit limb first.
 * Generated with pi = 16*atan(1/5) - 4*atan(1/239), 430 decimal digits.
 * Even at DBL_MAX the discarded tail affects the quotient by < 2^-128.
 * Only the large-angle path uses this table; audio angles use split pi/2. */
static const unsigned short two_over_pi[72] = {
    0x5D08, 0x1F8D, 0x5FB1, 0x6BFB, 0x92EA, 0x8A52, 0x39F7, 0x3D07,
    0xF17B, 0xEBE5, 0xBAC7, 0x7527, 0xEA2D, 0x9E5F, 0x3F66, 0x4F46,
    0x09B7, 0x27CB, 0x7ECF, 0x6D36, 0x6D1F, 0x5A0A, 0x118B, 0xEF2F,
    0x980F, 0xDE05, 0x97FF, 0x1FF8, 0x283B, 0xBDF9, 0x5F8B, 0x9C84,
    0x39F4, 0x8353, 0xD639, 0x3991, 0x7E41, 0xB45F, 0x7026, 0xE99C,
    0x4484, 0x2EBB, 0x35F5, 0xE882, 0xA73E, 0xB129, 0xEB1C, 0xFE1D,
    0x921C, 0x09D1, 0x2EEA, 0x0649, 0xD2E0, 0x424D, 0x6E3A, 0xB724,
    0xC561, 0xDEBB, 0x63AB, 0xFE51, 0x9041, 0x3C43, 0x9599, 0xDB62,
    0xDDC0, 0xF534, 0x57D1, 0xFC27, 0x1529, 0x4E44, 0x836E, 0xA2F9,
};

static int product_bit(const unsigned short *product, int bit) {
    if (bit < 0 || bit >= 1216) { return 0; }
    return (product[bit / 16] >> (bit & 15)) & 1;
}

/* Return fractional x/(pi/2) in [-1/2, 1/2], and nearest quadrant mod four.
 * Inputs to this slow path must be finite and |x| >= 2^20. */
unsigned long long __rcc_f64_reduce_pi2(unsigned long long a, long *quadrant) {
    unsigned short product[76];
    unsigned long long mantissa;
    unsigned long long fraction;
    unsigned long long sign;
    unsigned long carry;
    unsigned long word;
    unsigned long sum;
    int point;
    int round_up;
    int q;
    int first;
    int bit;
    int i;
    int j;
    int sticky;
    sign = a & SIGN;
    mantissa = (a & FRAC) | (1ULL << 52);
    point = 2227 - (int)((a >> 52) & 2047ULL);
    for (i = 0; i < 76; i++) { product[i] = 0; }
    for (i = 0; i < 4; i++) {
        word = (unsigned long)((mantissa >> (16 * i)) & 65535ULL);
        carry = 0;
        for (j = 0; j < 72; j++) {
            sum = word * (unsigned long)two_over_pi[j] + (unsigned long)product[i+j] + carry;
            product[i+j] = (unsigned short)sum;
            carry = sum >> 16;
        }
        product[i+72] = (unsigned short)carry;
    }
    q = product_bit(product, point) | (product_bit(product, point + 1) << 1);
    round_up = product_bit(product, point - 1);
    if (round_up) {
        q = (q + 1) & 3;
        carry = 1;
        for (i = 0; i < 76; i++) {
            sum = (unsigned long)(product[i] ^ 65535U) + carry;
            product[i] = (unsigned short)sum;
            carry = sum >> 16;
        }
    }
    if (sign) { q = (0 - q) & 3; }
    *quadrant = (long)q;
    first = point - 1;
    while (first >= 0 && !product_bit(product, first)) { first = first - 1; }
    if (first < 0) { return sign; }
    fraction = 0;
    for (i = 0; i < 64; i++) {
        fraction = (fraction << 1) | (unsigned long long)product_bit(product, first - i);
    }
    sticky = 0;
    for (bit = first - 64; bit >= 0; bit--) { sticky = sticky | product_bit(product, bit); }
    fraction = fraction | (unsigned long long)sticky;
    fraction = __rcc_f64_from_u64(fraction);
    fraction = fraction - ((unsigned long long)(point - first + 63) << 52);
    if (round_up) { sign = sign ^ SIGN; }
    return sign | fraction;
}
