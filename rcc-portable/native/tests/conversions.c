/* Numeric conversion behavior follows RCC native's saturating contract. */
static double signed32(long x) { return (double)x; }
static double unsigned64(unsigned long long x) { return (double)x; }
static float signed64(long long x) { return (float)x; }
static long to_signed32(double x) { return (long)x; }
static unsigned long to_unsigned32(float x) { return (unsigned long)x; }
static long long to_signed64(double x) { return (long long)x; }
static unsigned long long to_unsigned64(double x) { return (unsigned long long)x; }
int main(void)
{
    union { double f; unsigned long long bits; } d;
    union { float f; unsigned long bits; } f;
    if (signed32(-2147483647L - 1) != -2147483648.0) return 1;
    d.f = unsigned64(18446744073709551615ULL);
    if (d.bits != 0x43f0000000000000ULL) return 2;
    f.f = signed64(-9223372036854775807LL - 1);
    if (f.bits != 0xdf000000UL) return 3;
    if (to_signed32(-1234.75) != -1234L) return 4;
    if (to_unsigned32(1234.75f) != 1234UL) return 5;
    if (to_signed64(-4294967296.5) != -4294967296LL) return 6;
    /* Check conversion independently of the SDK decimal parser's rounding. */
    d.bits = 0x43e0000000000000ULL;
    if (to_unsigned64(d.f) != 9223372036854775808ULL) return 7;
    d.bits = 0x7ff0000000000000ULL;
    if (to_signed32(d.f) != 2147483647L || to_unsigned64(d.f) != 18446744073709551615ULL) return 8;
    d.bits = 0xfff0000000000000ULL;
    if (to_signed64(d.f) != (-9223372036854775807LL - 1) || to_unsigned64(d.f) != 0) return 9;
    d.bits = 0x7ff800123456789aULL;
    if (to_signed32(d.f) || to_signed64(d.f) || to_unsigned64(d.f)) return 10;
    if (to_signed32(2147483648.0) != 2147483647L) return 11;
    d.bits = 0x43e0000000000000ULL;
    if (to_signed64(d.f) != 9223372036854775807LL) return 12;
    if (to_unsigned64(18446744073709551616.0) != 18446744073709551615ULL) return 13;
    return 0;
}
