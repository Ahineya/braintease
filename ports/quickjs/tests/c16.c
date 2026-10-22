/* SPDX-License-Identifier: MIT
 * Actual QuickJS utility code and inline value representation, not JS eval.
 */
#include <stdio.h>
#include <stdlib.h>
#include "cutils.h"
#include "quickjs.h"

static unsigned checks, failures;
static void check(int ok, const char *label)
{
    ++checks;
    if (!ok) { ++failures; printf("FAIL %s\n", label); }
}

static void *deny(void *opaque, void *ptr, size_t size)
{
    (void)opaque; (void)ptr; (void)size;
    return NULL;
}

static long compare(const void *a, const void *b, void *opaque)
{
    const long *x = a, *y = b;
    (void)opaque;
    return *x < *y ? -1L : *x > *y ? 1L : 0L;
}

int main(void)
{
    qjs_octet_t bytes[16], *allocation, *cross;
    const qjs_octet_t *end;
    unsigned long offset, i;
    DynBuf buffer, denied;
    JSValue value;
    uint32_t *object;
    JSRefCountHeader *ref;
    long sorted[129];
    int float_tags_ok = 1;
    union { double d; uint64_t bits; } number;

    check(CHAR_BIT == 16 && sizeof(int) == 1 && sizeof(long) == 2, "C16 ABI");
    check(sizeof(JSValue) == 4 && sizeof(double) == 4, "64-bit value and double");
    value = JS_NewInt32(NULL, 70000L);
    check(JS_VALUE_GET_TAG(value) == JS_TAG_INT && JS_VALUE_GET_INT(value) == 70000L,
          "value integer beyond signed C int");
    value = JS_NewInt32(NULL, (-2147483647L - 1));
    check(JS_VALUE_GET_INT(value) == (-2147483647L - 1), "value signed 32-bit minimum");
    value = JS_NewUint32(NULL, 4294967295UL);
    check(JS_IsNumber(value) && JS_VALUE_GET_FLOAT64(value) == 4294967295.0,
          "unsigned 32-bit value becomes double");
    number.bits = 0x8000000000000000ULL;
    value = JS_NewFloat64(NULL, number.d);
    number.d = JS_VALUE_GET_FLOAT64(value);
    check(JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(value)) && number.bits == 0x8000000000000000ULL,
          "negative zero survives NaN boxing");
    number.bits = 0x7ff8000000000001ULL;
    check(JS_VALUE_IS_NAN(JS_NewFloat64(NULL, number.d)), "NaN normalization");
    /* These doubles deliberately give NaN-box tag low bits equal to object
     * tags. Narrowing the tag to C16 int falsely treats numbers as pointers.
     */
    for (i = 0; i < 64; ++i) {
        number.bits = ((uint64_t)(0x3ff80000UL + i) << 32) | 1ULL;
        value = JS_NewFloat64(NULL, number.d);
        if (!JS_IsNumber(value) || JS_IsBigInt(NULL, value) || JS_IsString(value) ||
            JS_IsObject(value) || JS_VALUE_HAS_REF_COUNT(value)) float_tags_ok = 0;
    }
    check(float_tags_ok, "floating tags never alias reference tags");
    check(JS_IsException(JS_EXCEPTION) && JS_IsUndefined(JS_UNDEFINED) && JS_IsNull(JS_NULL),
          "special value tags");
    object = malloc(3 * sizeof(uint32_t));
    if (!object) return 2;
    ref = (JSRefCountHeader *)object;
    ref->ref_count = 70000L;
    value = JS_MKPTR(JS_TAG_OBJECT, object + 1);
    check(JS_VALUE_GET_PTR(value) == (void *)(object + 1) && __js_rc(object + 1) == ref,
          "banked object pointer and reference header");
    value = JS_DupValue(NULL, value);
    check(ref->ref_count == 70001L && JS_IsObject(value), "32-bit reference count");
    free(object);

    put_u16(bytes + 1, 0x89abU);
    check(bytes[1] == 0xab && bytes[2] == 0x89 && get_u16(bytes + 1) == 0x89abUL,
          "unaligned 16-bit bytecode operand");
    check(get_i16(bytes + 1) == -30293L, "signed 16-bit operand");
    put_u32(bytes + 1, 0x89abcdefUL);
    check(bytes[1] == 0xef && bytes[4] == 0x89 && get_u32(bytes + 1) == 0x89abcdefUL,
          "unaligned 32-bit bytecode operand");
    check(get_i32(bytes + 1) == -1985229329L, "signed 32-bit operand");
    put_u64(bytes + 1, 0xfedcba9876543210ULL);
    check(bytes[1] == 0x10 && bytes[8] == 0xfe && get_u64(bytes + 1) == 0xfedcba9876543210ULL,
          "unaligned 64-bit bytecode operand");
    put_u8(bytes, 511U);
    check(*bytes == 255U && get_i8(bytes) == -1L, "octet truncation and sign extension");
    *bytes = 128;
    check(get_i8(bytes) == -128L, "signed eight-bit minimum");

    allocation = malloc(128000UL);
    if (!allocation) return 2;
    offset = 64000UL - ((uintptr_t)allocation & 65535UL);
    if (offset < 3) offset += 64000UL;
    cross = allocation + offset - 3;
    check(((uintptr_t)cross >> 16) != ((uintptr_t)(cross + 7) >> 16), "operand crosses a bank");
    put_u64(cross, 0xfedcba9876543210ULL);
    check(get_u64(cross) == 0xfedcba9876543210ULL, "bank-spanning bytecode read/write");
    free(allocation);

    check(unicode_to_utf8(bytes, 0x1f600UL) == 4 && bytes[0] == 0xf0 && bytes[3] == 0x80,
          "supplementary Unicode encoding");
    end = NULL;
    check(unicode_from_utf8(bytes, 4, &end) == 0x1f600L && end == bytes + 4,
          "supplementary Unicode decoding");
    bytes[0] = 0xc0; bytes[1] = 0x80; end = NULL;
    check(unicode_from_utf8(bytes, 2, &end) == -1L && end == NULL, "overlong sequence rejection");
    check(from_surrogate(0xd83dUL, 0xde00UL) == 0x1f600UL, "UTF-16 surrogate pair");
    check(clz32(1UL) == 31L && ctz32(0x80000000UL) == 31L &&
          clz64(1ULL) == 63L && ctz64(1ULL << 63) == 63L, "32/64-bit bit scans");
    {
        unsigned bit;
        for (bit = 0; bit < 32; ++bit) {
            uint32_t value = (uint32_t)1 << bit;
            check(clz32(value) == 31L - bit && ctz32(value) == bit, "all 32-bit scan positions");
            check(clz32(value | (value - 1)) == 31L - bit, "32-bit leading scan with lower bits");
        }
        for (bit = 0; bit < 64; ++bit) {
            uint64_t value = (uint64_t)1 << bit;
            check(clz64(value) == 63L - bit && ctz64(value) == bit, "all 64-bit scan positions");
            check(clz64(value | (value - 1)) == 63L - bit, "64-bit leading scan with lower bits");
        }
        check(clz32(0) == 32 && ctz32(0) == 32 && clz64(0) == 64 && ctz64(0) == 64,
              "zero scans terminate with the operand width");
    }

    dbuf_init(&buffer);
    check(dbuf_put_u16(&buffer, 0x89abU) == 0 && buffer.size == 2 &&
          get_u16(buffer.buf) == 0x89abUL, "slow dynamic buffer operand");
    check(dbuf_claim(&buffer, 32) == 0 && dbuf_put_u64(&buffer, 0xfedcba9876543210ULL) == 0 &&
          get_u64(buffer.buf + 2) == 0xfedcba9876543210ULL, "fast dynamic buffer operand");
    dbuf_free(&buffer);
    dbuf_init(&buffer);
    for (i = 0; i < 90000UL; ++i) {
        if (dbuf_putc(&buffer, (qjs_octet_t)(i & 255U))) { ++failures; break; }
    }
    check(buffer.size == 90000UL && buffer.buf[70000UL] == (70000UL & 255U),
          "dynamic buffer grows across banks");
    dbuf_free(&buffer);
    dbuf_init2(&denied, NULL, deny);
    check(dbuf_putc(&denied, 42) == -1L && dbuf_error(&denied), "allocation failure propagation");
    dbuf_free(&denied);

    for (i = 0; i < 129; ++i) sorted[i] = 70000L + (long)(128 - i);
    rqsort(sorted, 129, sizeof(long), compare, NULL);
    check(sorted[0] == 70000L && sorted[128] == 70128L, "utility sort with C16 object sizes");
    printf("QUICKJS_C16 checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
