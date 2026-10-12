#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef BANK_SIZE
#define BANK_SIZE 64000
#endif

/* Exercise observable behavior across the optimizer's block/loop boundaries.
 * Counts stay small; the heap accesses cross banks without a long fill loop. */
static volatile unsigned short shared;
static unsigned short call_count;
static unsigned short palette[4] = {11, 22, 33, 44};

static int fail(int number)
{
    printf("optimizer failure %d\n", number);
    return 1;
}

static unsigned short mutate_shared(void)
{
    ++call_count;
    return ++shared;
}

static uint32_t alias_loop(unsigned short *read, unsigned short *write)
{
    uint32_t sum = 0;
    unsigned short i;
    for (i = 0; i < 4; ++i) {
        sum += (uint32_t)*read * 9UL + 5UL;
        *write += 1;
    }
    return sum;
}

static uint32_t calls_and_loads(void)
{
    uint32_t sum = 0;
    unsigned short i;
    for (i = 0; i < 4; ++i) {
        sum += shared;
        mutate_shared();
        sum += shared;
    }
    return sum;
}

static uint32_t zero_trip(unsigned short count, unsigned short divisor)
{
    uint32_t sum = 19;
    unsigned short i;
    /* Division and the call are legal only when the loop actually executes. */
    for (i = 0; i < count; ++i) {
        sum += 120UL / divisor;
        mutate_shared();
    }
    return sum;
}

static uint32_t nested_loops(unsigned short count)
{
    uint32_t sum = 0;
    unsigned short outer, inner;
    for (outer = 0; outer < count; ++outer) {
        inner = 0;
        while (inner < 5) {
            ++inner;
            if (inner == 2) continue;
            if (outer == 2 && inner == 4) break;
            sum += (uint32_t)(outer + 1) * 17UL + inner;
        }
    }
    return sum;
}

static uint32_t live_across_branches(uint32_t seed, unsigned short count)
{
    uint32_t a = seed + 0x12340000UL;
    uint32_t b = seed + 0x23450000UL;
    uint32_t c = seed + 0x34560000UL;
    uint32_t d = seed + 0x45670000UL;
    uint32_t sum = 0;
    unsigned short i;
    for (i = 0; i < count; ++i) {
        if (i & 1) sum += a ^ b;
        else sum += c ^ d;
        sum += palette[i & 3];
        a += 3;
        b += 5;
        c += 7;
        d += 9;
    }
    return sum ^ (a << 16) ^ (b >> 16) ^ (c << 1) ^ (d >> 1);
}

static int banked_accesses(void)
{
    uint32_t bank = BANK_SIZE;
    unsigned short local[4] = {3, 5, 7, 9};
    unsigned short *heap = malloc((2UL * bank + 8UL) * sizeof(*heap));
    unsigned short *cursor;
    unsigned short i;
    if (!heap) return 0;
    heap[0] = 101;
    heap[bank - 1UL] = 202;
    heap[bank] = 303;
    heap[bank + 1UL] = 404;
    heap[2UL * bank - 1UL] = 505;
    heap[2UL * bank + 3UL] = 606;
    cursor = heap + bank + 1UL;
    if (cursor[-2] != 202 || cursor[-1] != 303 || *cursor != 404) return 0;
    cursor += bank + 2UL;
    if (*cursor != 606 || cursor[-4] != 505) return 0;
    cursor -= 2UL * bank + 3UL;
    if (cursor != heap || *cursor != 101) return 0;
    /* The same loop reaches global, stack and dynamic pointer banks. */
    for (i = 0; i < 4; ++i) heap[bank + i] = palette[i] + local[i];
    if (heap[bank] != 14 || heap[bank + 1UL] != 27
        || heap[bank + 2UL] != 40 || heap[bank + 3UL] != 53) return 0;
    free(heap);
    return 1;
}

int main(void)
{
    unsigned short alias = 7;
    uint32_t wrap;
    unsigned short count, sum;
    if ((unsigned short)(60000U + 10000U) != 4464U) return fail(1);
    wrap = (uint32_t)0xfffffff0UL + (uint32_t)0x30UL;
    if (wrap != 0x20UL) return fail(2);
    if ((uint32_t)320 * 200 != 64000UL) return fail(3);
    if (-7L / 3L != -2L || -7L % 3L != -1L) return fail(4);
    if (((uint32_t)0x12345678UL >> 16) != 0x1234UL) return fail(5);
    if ((uint32_t)((uint32_t)0x12345678UL << 17) != 0xacf00000UL) return fail(6);
    if (alias_loop(&alias, &alias) != 326UL || alias != 11) return fail(7);
    shared = 2;
    call_count = 0;
    if (calls_and_loads() != 32UL || call_count != 4 || shared != 6) return fail(8);
    if (zero_trip(0, 0) != 19UL || call_count != 4 || shared != 6) return fail(9);
    if (zero_trip(2, 3) != 99UL || call_count != 6 || shared != 8) return fail(10);
    if (nested_loops(4) != 621UL) return fail(11);
    count = 0;
    sum = 0;
    do {
        ++count;
        if (count == 2) continue;
        sum += count * 7;
    } while (count < 4);
    if (sum != 56 || count != 4) return fail(12);
    if (live_across_branches(5UL, 6) != 0xadeea3c7UL) return fail(13);
    if (!banked_accesses()) return fail(14);
    puts("optimizer semantics ok");
    return 0;
}
