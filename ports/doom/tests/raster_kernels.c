/* Independent C oracle for the existing-ISA wall and floor inner loops. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "render_fast.h"

#ifndef RIPPLE
void Doom_DrawColumn(unsigned char *dest, const unsigned char *source,
                     const unsigned char *map, uint32_t frac, uint32_t step,
                     unsigned int count)
{
    while (count--) {
        *dest = map[source[(frac >> 16) & 127]];
        dest += 320;
        frac += step;
    }
}
void Doom_DrawSpan(unsigned char *dest, const unsigned char *source,
                   const unsigned char *map, uint32_t position, uint32_t step,
                   unsigned int count)
{
    while (count--) {
        *dest++ = map[source[((position >> 4) & 4032) | (position >> 26)]];
        position += step;
    }
}
void Doom_DrawColumnLow(unsigned char *dest, const unsigned char *source,
                        const unsigned char *map, uint32_t frac, uint32_t step,
                        unsigned int count)
{
    while (count--) {
        dest[1] = dest[0] = map[source[(frac >> 16) & 127]];
        dest += 320;
        frac += step;
    }
}
void Doom_DrawSpanLow(unsigned char *dest, const unsigned char *source,
                      const unsigned char *map, uint32_t position, uint32_t step,
                      unsigned int count)
{
    while (count--) {
        dest[1] = dest[0] = map[source[((position >> 4) & 4032) | (position >> 26)]];
        dest += 2;
        position += step;
    }
}
#endif

static unsigned char global_source[4096];
static unsigned char global_map[256];
static uint32_t random_state = 1;

static uint32_t next_random(void)
{
    random_state = random_state * 1664525UL + 1013904223UL;
    return random_state;
}

static unsigned char *at_offset(unsigned char *allocation, unsigned int offset)
{
#ifdef RIPPLE
    /* Three allocated banks leave room both before and after the requested
     * position, regardless of the allocator's starting cell. */
    uintptr_t bank = ((uintptr_t)allocation >> 16) + 1UL;
    return (unsigned char *)((bank << 16) | offset);
#else
    return allocation + 64000UL + offset;
#endif
}

static void fill(unsigned char *source, unsigned char *map)
{
    unsigned int i;
    for (i = 0; i < 4096; ++i) source[i] = (i * 17U + (i >> 2)) & 255U;
    for (i = 0; i < 256; ++i) map[i] = (i * 73U + 9U) & 255U;
}

static void check(unsigned char *dest, const unsigned char *source,
                  const unsigned char *map, unsigned int span, unsigned int low,
                  uint32_t position, uint32_t step, unsigned int count)
{
    unsigned char expected[200];
    uint32_t cursor = position;
    unsigned int i, stride = span ? (low ? 2U : 1U) : 320U;
    for (i = 0; i < count; ++i) {
        unsigned int spot;
        if (span) spot = ((cursor >> 4) & 4032UL) | (cursor >> 26);
        else spot = (cursor >> 16) & 127UL;
        expected[i] = map[source[spot]];
        dest[(uint32_t)i * stride] = expected[i] ^ 255U;
        if (low) dest[(uint32_t)i * stride + 1] = expected[i] ^ 255U;
        cursor += step;
    }
    dest[-1] = 91;
    dest[(uint32_t)count * stride] = 37;
    if (low) {
        if (span) Doom_DrawSpanLow(dest, source, map, position, step, count);
        else Doom_DrawColumnLow(dest, source, map, position, step, count);
    } else {
        if (span) Doom_DrawSpan(dest, source, map, position, step, count);
        else Doom_DrawColumn(dest, source, map, position, step, count);
    }
    for (i = 0; i < count; ++i) {
        if (dest[(uint32_t)i * stride] != expected[i]
            || (low && dest[(uint32_t)i * stride + 1] != expected[i])) {
            printf("RASTER mismatch span=%u low=%u count=%u pixel=%u\n", span, low, count, i);
            exit(1);
        }
    }
    if (dest[-1] != 91 || dest[(uint32_t)count * stride] != 37) {
        puts("RASTER guard overwritten");
        exit(1);
    }
}

int main(void)
{
    unsigned char *storage = malloc(192000UL);
    unsigned char *colors = malloc(192000UL);
    unsigned char *screen = malloc(192000UL);
    unsigned char local_map[256];
    unsigned char local_dest[402];
    unsigned int placement, sample, i, calls = 0;
    if (!storage || !colors || !screen) return 1;
    fill(global_source, global_map);
    for (i = 0; i < 256; ++i) local_map[i] = global_map[i];
    Doom_DrawColumn(NULL, NULL, NULL, 0, 0, 0);
    Doom_DrawSpan(NULL, NULL, NULL, 0, 0, 0);
    Doom_DrawColumnLow(NULL, NULL, NULL, 0, 0, 0);
    Doom_DrawSpanLow(NULL, NULL, NULL, 0, 0, 0);
    for (placement = 0; placement < 8; ++placement) {
        unsigned char *source = at_offset(storage, placement & 1 ? 63950U : 200U);
        unsigned char *map = at_offset(colors, placement & 2 ? 63920U : 200U);
        unsigned char *dest = at_offset(screen, placement & 4 ? 63950U : 200U);
        fill(source, map);
        for (sample = 0; sample < 32; ++sample) {
            uint32_t position = next_random(), step = next_random();
            unsigned int count = sample < 3 ? sample : 200;
            if (sample == 3) step = 0;
            if (sample == 4) { position = 0xffffffffUL; step = 1; }
            if (sample == 5) { position = 0; step = 0xffffffffUL; }
            check(dest, source, map, 0, 0, position, step, count);
            check(dest, source, map, 1, 0, position, step, count);
            check(dest, source, map, 0, 1, position, step, count);
            check(dest, source, map, 1, 1, position, step, count);
            calls += 4;
        }
    }
    /* These arguments exercise GP and SB provenance tags, including a stack
     * destination. Their small counts keep every stack access in its object. */
    for (sample = 0; sample < 32; ++sample) {
        uint32_t position = next_random(), step = next_random();
        check(local_dest + 1, global_source, global_map, 1, 0, position, step, 200);
        check(local_dest + 1, global_source, local_map, 1, 0, position, step, 200);
        check(local_dest + 1, global_source, global_map, 1, 1, position, step, 200);
        check(local_dest + 1, global_source, local_map, 1, 1, position, step, 200);
        calls += 4;
    }
    printf("RASTER OK %u cases: columns, spans, both detail modes, split banks, tags, wraparound\n", calls);
    return 0;
}
