/* Real Doom arithmetic and canonical tables, tested on native C and Ripple.
 * Expected vectors are explicit; the executable does not calculate an oracle
 * using the same arithmetic it is testing. Build with DOOM_GENERATED_TABLES. */
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "m_fixed.h"
#include "tables.h"

typedef struct FixedVector {
    int32_t a;
    int32_t b;
    int32_t expected;
} FixedVector;

static const FixedVector mul_vectors[] = {
    {0, INT32_MIN, 0},
    {INT32_C(65536), INT32_C(65536), INT32_C(65536)},
    {-INT32_C(65536), INT32_C(65536), -INT32_C(65536)},
    {INT32_C(32768), INT32_C(32768), INT32_C(16384)},
    {INT32_C(98304), -INT32_C(163840), -INT32_C(245760)},
    {-1, INT32_C(32768), -1},
    {-1, -INT32_C(32768), 0},
    {INT32_MAX, INT32_C(65536), INT32_MAX},
    {INT32_MIN, INT32_C(65536), INT32_MIN},
    {INT32_MIN, -INT32_C(65536), INT32_MIN},
    {INT32_C(2147418112), INT32_C(2147418112), INT32_C(65536)},
    {INT32_MAX, INT32_MAX, -INT32_C(65536)},
    {INT32_MIN, INT32_MIN, 0},
    {INT32_MIN, 1, -INT32_C(32768)}
};

static const FixedVector div_vectors[] = {
    {INT32_C(65536), INT32_C(65536), INT32_C(65536)},
    {-INT32_C(65536), INT32_C(65536), -INT32_C(65536)},
    {INT32_C(65536), -INT32_C(65536), -INT32_C(65536)},
    {-INT32_C(65536), -INT32_C(65536), INT32_C(65536)},
    {INT32_C(65536), INT32_C(196608), INT32_C(21845)},
    {-INT32_C(65536), INT32_C(196608), -INT32_C(21845)},
    {INT32_C(131072), INT32_C(65536), INT32_C(131072)},
    {INT32_MAX, 1, INT32_MAX},
    {INT32_MIN, 1, INT32_MIN},
    {INT32_MIN, -1, INT32_MAX},
    {INT32_MIN, INT32_MIN, INT32_C(65536)},
    {INT32_MAX, INT32_MAX, INT32_C(65536)},
    {0, 0, INT32_MAX},
    {1, 0, INT32_MAX},
    {-1, 0, INT32_MIN},
    {0, -1, 0},
    {0, INT32_MIN, 0},
    {INT32_C(16383), 1, INT32_C(1073676288)},
    {INT32_C(16384), 1, INT32_MAX},
    {-INT32_C(16384), 1, INT32_MIN}
};

static int failures;

static void expect(const char *name, int32_t actual, int32_t expected)
{
    if (actual != expected) {
        printf("FAIL %s: %ld expected %ld\n", name, (long) actual, (long) expected);
        ++failures;
    }
}

static uint32_t hash_word(uint32_t hash, uint32_t word)
{
    unsigned int octet;
    for (octet = 0; octet < 4; ++octet) {
        hash = (hash ^ (word & UINT32_C(255))) * UINT32_C(16777619);
        word >>= 8;
    }
    return hash;
}

static uint32_t hash_fixed(const fixed_t *values, unsigned int count)
{
    uint32_t hash = UINT32_C(2166136261);
    unsigned int i;
    for (i = 0; i < count; ++i)
        hash = hash_word(hash, (uint32_t) values[i]);
    return hash;
}

int main(void)
{
    unsigned int i;
    uint32_t sine_hash;
    uint32_t tangent_hash;
    uint32_t angle_hash = UINT32_C(2166136261);
    const fixed_t *original;

    expect("fixed width", (int32_t) (sizeof(fixed_t) * CHAR_BIT), 32);
    expect("angle width", (int32_t) (sizeof(angle_t) * CHAR_BIT), 32);
    expect("FRACUNIT", FRACUNIT, INT32_C(65536));
    for (i = 0; i < sizeof(mul_vectors) / sizeof(mul_vectors[0]); ++i)
        expect("FixedMul", FixedMul(mul_vectors[i].a, mul_vectors[i].b), mul_vectors[i].expected);
    for (i = 0; i < sizeof(div_vectors) / sizeof(div_vectors[0]); ++i)
        expect("FixedDiv", FixedDiv(div_vectors[i].a, div_vectors[i].b), div_vectors[i].expected);

    expect("SlopeDiv zero", SlopeDiv(0, 0), 2048);
    expect("SlopeDiv small denominator", SlopeDiv(1, 511), 2048);
    expect("SlopeDiv first", SlopeDiv(1, 512), 4);
    expect("SlopeDiv half", SlopeDiv(256, 512), 1024);
    expect("SlopeDiv full", SlopeDiv(512, 512), 2048);
    expect("SlopeDiv clamp", SlopeDiv(1000, 512), 2048);
    expect("SlopeDiv wide", SlopeDiv(UINT32_C(65536), UINT32_C(131072)), 1024);
    expect("SlopeDiv wrap", SlopeDiv(UINT32_C(0x20000000), 4096), 0);
    expect("SlopeDiv maximum", SlopeDiv(UINT32_MAX, UINT32_MAX), 256);

    if (!Doom_InitTables()) {
        printf("FAIL table allocation\n");
        return 1;
    }
    original = finesine;
    if (!Doom_InitTables() || original != finesine) {
        printf("FAIL table initialization is not idempotent\n");
        ++failures;
    }
    expect("sine first", finesine[0], 25);
    expect("sine quarter", finesine[2560], INT32_C(60537));
    expect("sine negative", finesine[5120], -INT32_C(46358));
    expect("sine last", finesine[10239], INT32_C(65535));
    expect("cosine alias", finecosine[0], finesine[FINEANGLES / 4]);
    expect("tangent first", finetangent[0], -INT32_C(170910304));
    expect("tangent midpoint", finetangent[2048], 25);
    expect("tangent last", finetangent[4095], INT32_C(170910304));
    expect("angle first", (int32_t) tantoangle[0], 0);
    expect("angle midpoint", (int32_t) tantoangle[1024], INT32_C(316933408));
    expect("angle last", (int32_t) tantoangle[2048], INT32_C(536870912));

    sine_hash = hash_fixed(finesine, 10240);
    tangent_hash = hash_fixed(finetangent, 4096);
    for (i = 0; i < 2049; ++i)
        angle_hash = hash_word(angle_hash, tantoangle[i]);
    if (sine_hash != UINT32_C(0x6e970016)
        || tangent_hash != UINT32_C(0xd440d0e1)
        || angle_hash != UINT32_C(0x1c93684a)) {
        printf("FAIL table hashes: %lx %lx %lx\n", (unsigned long) sine_hash,
               (unsigned long) tangent_hash, (unsigned long) angle_hash);
        ++failures;
    }
    Doom_FreeTables();
    if (finesine != NULL || finecosine != NULL || finetangent != NULL || tantoangle != NULL) {
        printf("FAIL table teardown\n");
        ++failures;
    }
    if (failures != 0)
        return 1;
    printf("NUMERIC OK %lx %lx %lx\n", (unsigned long) sine_hash,
           (unsigned long) tangent_hash, (unsigned long) angle_hash);
    return 0;
}
