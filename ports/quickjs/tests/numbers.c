/* SPDX-License-Identifier: MIT
 * The same fixture runs against pristine host dtoa and the adapted guest dtoa.
 * Compare N:/P: output records as well as checking bit-exact round trips.
 */
#include <stdio.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include "cutils.h"
#include "dtoa.h"

static unsigned checks, failures;
static void check(int ok, const char *name)
{
    ++checks;
    if (!ok) { ++failures; printf("FAIL %s\n", name); }
}
static void hex64(uint64_t value)
{
    int shift;
    for (shift = 60; shift >= 0; shift -= 4)
        putchar("0123456789abcdef"[(unsigned)((value >> shift) & 15)]);
}

static void format(uint64_t bits, int radix, int digits, int flags)
{
    struct { uint64_t before; JSDTOATempMem mem; uint64_t after; } output;
    struct { uint64_t before; JSATODTempMem mem; uint64_t after; } input;
    struct { uint64_t before; char text[1024]; uint64_t after; } buffer;
    const uint64_t guard = 0x123456789abcdef0ULL;
    const char *end;
    double value = uint64_as_float64(bits), restored;
    long length, maximum;
    output.before = output.after = input.before = input.after = buffer.before = buffer.after = guard;
    maximum = js_dtoa_max_len(value, radix, digits, flags);
    if (maximum >= (long)sizeof(buffer.text)) { check(0, "format bound"); return; }
    length = js_dtoa(buffer.text, value, radix, digits, flags, &output.mem);
    check(length >= 0 && length <= maximum && (size_t)length == strlen(buffer.text), "format length");
    printf("N:"); hex64(bits);
    printf(":%ld:%ld:%ld:%s\n", (long)radix, (long)digits, (long)flags, buffer.text);
    if ((flags & JS_DTOA_FORMAT_MASK) == JS_DTOA_FORMAT_FREE &&
        (bits & 0x7ff0000000000000ULL) != 0x7ff0000000000000ULL) {
        restored = js_atod(buffer.text, &end, radix, 0, &input.mem);
        check(float64_as_uint64(restored) == bits && *end == 0, "round trip");
    }
    check(output.before == guard && output.after == guard && input.before == guard &&
          input.after == guard && buffer.before == guard && buffer.after == guard, "numeric workspace guards");
}

static void parse(const char *text, int radix, int flags, uint64_t expected, long consumed)
{
    JSATODTempMem memory;
    const char *end;
    uint64_t bits = float64_as_uint64(js_atod(text, &end, radix, flags, &memory));
    int is_nan = (bits & 0x7fffffffffffffffULL) > 0x7ff0000000000000ULL;
    check(expected == 0x7ff8000000000000ULL ? is_nan : bits == expected, "parsed number bits");
    if (consumed >= 0) check(end - text == consumed, "parse endpoint");
    printf("P:%s:", text); hex64(bits); printf(":%ld\n", (long)(end - text));
}

int main(void)
{
    static const uint64_t values[] = {
        0, 0x8000000000000000ULL, 1, 0x000fffffffffffffULL,
        0x0010000000000000ULL, 0x3ff0000000000000ULL, 0xbff0000000000000ULL,
        0x3fb999999999999aULL, 0x400921fb54442d18ULL, 0x4340000000000000ULL,
        0x433fffffffffffffULL, 0x7ef0000000000000ULL, 0x7fefffffffffffffULL,
        0x7ff0000000000000ULL, 0xfff0000000000000ULL, 0x7ff8000000000000ULL
    };
    unsigned i;
    uint32_t state = 0x13579bdfUL, high, low;
    char integer[64];
    for (i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
        format(values[i], 10, 0, JS_DTOA_MINUS_ZERO);
    for (i = 0; i < 64; ++i) {
        state = state * 1664525UL + 1013904223UL; high = state;
        state = state * 1664525UL + 1013904223UL; low = state;
        if ((high & 0x7ff00000UL) == 0x7ff00000UL) high ^= 0x00100000UL;
        format(((uint64_t)high << 32) | low, 10, 0, JS_DTOA_MINUS_ZERO);
    }
    for (i = 2; i <= 36; ++i)
        format(0x3fb999999999999aULL, i, 0, JS_DTOA_MINUS_ZERO);
    format(0x3ff4000000000000ULL, 10, 1, JS_DTOA_FORMAT_FRAC); /* 1.25 -> 1.3 */
    format(0x4005666666666666ULL, 10, 2, JS_DTOA_FORMAT_FRAC); /* 2.675 -> 2.67 */
    format(0x7fefffffffffffffULL, 10, 101, JS_DTOA_FORMAT_FIXED);
    format(0x7fefffffffffffffULL, 10, 101, JS_DTOA_FORMAT_FRAC);
    parse("0.1", 0, 0, 0x3fb999999999999aULL, 3);
    parse("9007199254740993", 0, 0, 0x4340000000000000ULL, 16);
    parse("5e-324", 0, 0, 1, 6);
    parse("1e309", 0, 0, 0x7ff0000000000000ULL, 5);
    parse("-0", 0, 0, 0x8000000000000000ULL, 2);
    parse("0x1p1008", 0, 0, 0x7ef0000000000000ULL, 8);
    parse("0b101010", 0, JS_ATOD_ACCEPT_BIN_OCT, 0x4045000000000000ULL, 8);
    parse("123_456", 0, JS_ATOD_ACCEPT_UNDERSCORES, 0x40fe240000000000ULL, 7);
    parse("12.5tail", 0, 0, 0x4029000000000000ULL, 4);
    parse("Infinity", 0, 0, 0x7ff0000000000000ULL, 8);
    parse("-Infinity", 0, 0, 0xfff0000000000000ULL, 9);
    parse("0xff", 0, JS_ATOD_INT_ONLY, 0x406fe00000000000ULL, 4);
    parse("0x", 0, 0, 0x7ff8000000000000ULL, -1);
    parse("1e+", 0, 0, 0x7ff8000000000000ULL, -1);
    parse("nothing", 0, 0, 0x7ff8000000000000ULL, -1);
    integer[u64toa(integer, 18446744073709551615ULL)] = 0;
    check(strcmp(integer, "18446744073709551615") == 0, "uint64 decimal conversion");
    integer[i64toa(integer, (-9223372036854775807LL - 1))] = 0;
    check(strcmp(integer, "-9223372036854775808") == 0, "int64 decimal conversion");
    printf("QUICKJS_NUMBERS checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
