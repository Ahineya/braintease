/* An instrumented oracle for pure logical pointer arithmetic, not a memory
 * runtime. The emitted object must not acquire any new runtime dependency. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t (*Step)(void *, uint64_t, uint64_t);
extern uint64_t rcc_user_gep1(void *, uint64_t, uint64_t);
extern uint64_t rcc_user_gep2(void *, uint64_t, uint64_t);
extern uint64_t rcc_user_gep4(void *, uint64_t, uint64_t);
extern uint64_t rcc_user_gep16(void *, uint64_t, uint64_t);
extern uint64_t rcc_user_gep32(void *, uint64_t, uint64_t);
static uint64_t banks, calls, polls, fasts;

static int reference(uint64_t p, uint64_t delta, uint64_t *result) {
    uint64_t bank = p >> 16, offset = p & 65535;
    if (bank >= banks || offset >= 64000) return 0;
    __int128 dense = (__int128)bank * 64000 + offset + (int64_t)delta;
    if (dense < 0 || dense >= (__int128)banks * 64000) return 0;
    *result = ((uint64_t)(dense / 64000) << 16) | (uint64_t)(dense % 64000);
    return 1;
}

uint64_t rcc_native_gep(void *ctx, uint64_t p, uint64_t delta) {
    uint64_t result;
    (void)ctx;
    calls++;
    if (!reference(p, delta, &result)) {
        fprintf(stderr, "checked-gep: rejected %llx + %lld\n",
                (unsigned long long)p, (long long)delta);
        exit(86);
    }
    return result;
}
uint64_t rcc_native_poll(void *ctx) { (void)ctx; polls++; return 0; }
uint64_t rcc_native_register(void *ctx, uint64_t token, uint64_t address) {
    (void)ctx; (void)token; (void)address; return 0;
}

static void check(Step step, uint64_t p, uint64_t index, unsigned stride, unsigned bits) {
    uint64_t raw_index = index;
    if (bits == 16) index &= 65535; /* narrowed GEP indices are unsigned */
    if (bits == 32) index = (uint64_t)(int64_t)(int32_t)index;
    uint64_t delta = index * stride, expected;
    if (!reference(p, delta, &expected)) return;
    uint64_t before = calls, before_polls = polls;
    uint64_t actual = step(NULL, p, raw_index);
    int fast = (p >> 16) < 256 && (expected >> 16) == (p >> 16);
    if (fast) fasts++;
    if (actual != expected || calls - before != (uint64_t)!fast || polls != before_polls) {
        fprintf(stderr, "GEP mismatch: p=%llx i=%llx stride=%u bits=%u result=%llx expected=%llx calls=%llu fast=%d polls=%llu\n",
                (unsigned long long)p, (unsigned long long)index, stride, bits,
                (unsigned long long)actual, (unsigned long long)expected,
                (unsigned long long)(calls - before), fast,
                (unsigned long long)(polls - before_polls));
        exit(1);
    }
}

uint64_t rcc_native_start(uint64_t entry) {
    (void)entry;
    banks = strtoull(getenv("GEP_BANKS"), NULL, 10);
    const char *fault = getenv("GEP_FAULT");
    if (fault) {
        uint64_t p = 2ULL << 16, delta = 0;
        if (!strcmp(fault, "bad-offset")) p |= 64000;
        else if (!strcmp(fault, "bad-offset-recovery")) { p |= 64000; delta = -1ULL; }
        else if (!strcmp(fault, "bad-bank")) p = banks << 16;
        else if (!strcmp(fault, "underflow")) { p = 0; delta = -1ULL; }
        else if (!strcmp(fault, "overflow")) { p = ((banks - 1) << 16) | 63999; delta = 1; }
        else if (!strcmp(fault, "min-delta")) delta = 1ULL << 63;
        else if (!strcmp(fault, "max-delta")) delta = INT64_MAX;
        else if (!strcmp(fault, "high-carrier")) p |= 1ULL << 48;
        else return 2;
        rcc_user_gep1(NULL, p, delta);
        return 3; /* invalid arithmetic must enter the checked helper */
    }
    uint64_t bank_cases[] = {0, 1, 2, 31, 32, 239, 240, 255, 256, 32767, 32768, 65535};
    uint64_t offsets[] = {0, 1, 31, 32, 511, 512, 63998, 63999};
    uint64_t deltas[] = {0, 1, -1ULL, 2, -2ULL, 512, -512ULL, 64000, -64000ULL,
                         65535, 0xffffffffULL, 1ULL << 63, INT64_MAX};
    struct { Step step; unsigned stride, bits; } steps[] = {
        {rcc_user_gep1, 1, 64}, {rcc_user_gep2, 2, 64}, {rcc_user_gep4, 4, 64},
        {rcc_user_gep16, 1, 16}, {rcc_user_gep32, 1, 32},
    };
    for (unsigned s = 0; s < sizeof(steps) / sizeof(*steps); s++) {
        for (unsigned b = 0; b < sizeof(bank_cases) / sizeof(*bank_cases); b++)
            for (unsigned o = 0; o < sizeof(offsets) / sizeof(*offsets); o++)
                for (unsigned d = 0; d < sizeof(deltas) / sizeof(*deltas); d++)
                    check(steps[s].step, (bank_cases[b] << 16) | offsets[o], deltas[d],
                          steps[s].stride, steps[s].bits);
        uint64_t random = 0x4a34b51dULL;
        for (unsigned i = 0; i < 20000; i++) {
            random ^= random << 13; random ^= random >> 7; random ^= random << 17;
            uint64_t bank = random % banks, offset = (random >> 24) % 64000;
            uint64_t index = (uint64_t)((int64_t)((random >> 40) % 128001) - 64000);
            check(steps[s].step, (bank << 16) | offset, index, steps[s].stride, steps[s].bits);
        }
    }
    if (!calls || !fasts) return 4; /* exercise both routes */
    puts("GEP differential paths passed");
    return 0;
}
