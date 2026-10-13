#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "alloc_fast.h"

/* Return aligned, deliberately dirty allocations with guards on both ends.
 * The production zeroing implementation below must clear reused-cell contents,
 * not merely rely on a fresh zero-filled heap. */
typedef union
{
    void *pointer;
    uint32_t number;
} storage_alignment_t;

#define GUARD_BYTES (16 * sizeof(storage_alignment_t))
static unsigned char *blocks[2];
static unsigned char *payloads[2];
static size_t sizes[2];
static unsigned int allocations;

static void fail(const char *message)
{
    puts(message);
    exit(1);
}

static void *storage_malloc(size_t bytes)
{
    unsigned char *block;
    if (allocations >= 2 || bytes > (size_t)-1 - 2 * GUARD_BYTES)
        fail("unexpected renderer allocation");
    block = malloc(bytes + 2 * GUARD_BYTES);
    if (block == NULL) fail("fixture backing allocation failed");
    memset(block, 91, bytes + 2 * GUARD_BYTES);
    blocks[allocations] = block;
    payloads[allocations] = block + GUARD_BYTES;
    sizes[allocations] = bytes;
    memset(block + GUARD_BYTES, 165, bytes);
    ++allocations;
    return block + GUARD_BYTES;
}

static void *storage_calloc(size_t count, size_t size)
{
    return Doom_Calloc(count, size);
}

/* Include the real helper under malloc interception: both native calloc and
 * Ripple's fast zero allocator then see identical dirty backing storage. */
#ifdef calloc
#undef calloc
#endif
#define malloc storage_malloc
#define calloc storage_calloc
#include "../src/alloc_fast.c"
#include "renderer_storage.inc"
#undef malloc
#undef calloc

void I_Error(char *message, ...)
{
    fail(message);
}

static int cells(const unsigned char *data, size_t count, unsigned int value)
{
    while (count != 0) {
        if (*data != value) return 0;
        ++data;
        --count;
    }
    return 1;
}

typedef struct
{
    unsigned char *data;
    size_t size;
    unsigned int value;
} region_t;

static region_t regions[17];
static unsigned int region_count;

static void region(void *data, size_t size, unsigned int allocation,
                   unsigned int initial)
{
    unsigned char *start = data;
    size_t offset;
    if (start < payloads[allocation]) fail("region starts outside slab");
    offset = (size_t)(start - payloads[allocation]);
    if (offset > sizes[allocation] || size > sizes[allocation] - offset)
        fail("region ends outside slab");
    if (!cells(start, size, initial)) fail("incorrect initial storage contents");
    if (region_count >= 17) fail("too many renderer regions");
    regions[region_count].data = start;
    regions[region_count].size = size;
    regions[region_count].value = region_count + 1;
    /* Filling every cell catches overlaps between arrays and truncated far
     * struct-member offsets, not just mistakes at the first/last element. */
    memset(start, region_count + 1, size);
    ++region_count;
}

#ifndef RIPPLE
struct align_int { char pad; int value; };
struct align_angle { char pad; angle_t value; };
struct align_pointer { char pad; lighttable_t *value; };
struct align_plane { char pad; visplane_t value; };
struct align_short { char pad; short value; };
struct align_fixed { char pad; fixed_t value; };
#define ALIGNED(pointer, kind) ((uintptr_t)(pointer) % offsetof(struct align_##kind, value) == 0)
#endif

int main(void)
{
    unsigned int i;
    size_t total = sizeof(renderer_view_storage_t) + sizeof(renderer_plane_storage_t);
    if (R_StorageBytes() != 0) fail("initial renderer storage accounting");
    R_InitViewStorage();
    R_InitPlanes();
    if (allocations != 2 || sizes[0] != sizeof(renderer_view_storage_t)
        || sizes[1] != sizeof(renderer_plane_storage_t) || R_StorageBytes() != total)
        fail("renderer slab sizes or allocation count");
    if (!cells(payloads[1], sizes[1], 0)) fail("plane slab was not fully zeroed");

    region(viewangletox, (FINEANGLES / 2) * sizeof(*viewangletox), 0, 165);
    region(xtoviewangle, (SCREENWIDTH + 1) * sizeof(*xtoviewangle), 0, 165);
    region(scalelight, LIGHTLEVELS * sizeof(*scalelight), 0, 165);
    region(scalelightfixed, MAXLIGHTSCALE * sizeof(*scalelightfixed), 0, 0);
    region(zlight, LIGHTLEVELS * sizeof(*zlight), 0, 165);
    region(visplanes, MAXVISPLANES * sizeof(*visplanes), 1, 0);
    region(openings, MAXOPENINGS * sizeof(*openings), 1, 0);
    region(floorclip, SCREENWIDTH * sizeof(*floorclip), 1, 0);
    region(ceilingclip, SCREENWIDTH * sizeof(*ceilingclip), 1, 0);
    region(spanstart, SCREENHEIGHT * sizeof(*spanstart), 1, 0);
    region(spanstop, SCREENHEIGHT * sizeof(*spanstop), 1, 0);
    region(yslope, SCREENHEIGHT * sizeof(*yslope), 1, 0);
    region(distscale, SCREENWIDTH * sizeof(*distscale), 1, 0);
    region(cachedheight, SCREENHEIGHT * sizeof(*cachedheight), 1, 0);
    region(cacheddistance, SCREENHEIGHT * sizeof(*cacheddistance), 1, 0);
    region(cachedxstep, SCREENHEIGHT * sizeof(*cachedxstep), 1, 0);
    region(cachedystep, SCREENHEIGHT * sizeof(*cachedystep), 1, 0);

#ifdef RIPPLE
    if (sizes[1] <= BANK_SIZE
        || (size_t)((unsigned char *)openings - (unsigned char *)visplanes) <= BANK_SIZE)
        fail("fixture no longer exercises far slab members");
#else
    if (!ALIGNED(viewangletox, int) || !ALIGNED(xtoviewangle, angle)
        || !ALIGNED(scalelight, pointer) || !ALIGNED(scalelightfixed, pointer)
        || !ALIGNED(zlight, pointer) || !ALIGNED(visplanes, plane)
        || !ALIGNED(openings, short) || !ALIGNED(floorclip, short)
        || !ALIGNED(ceilingclip, short) || !ALIGNED(spanstart, int)
        || !ALIGNED(spanstop, int) || !ALIGNED(yslope, fixed)
        || !ALIGNED(distscale, fixed) || !ALIGNED(cachedheight, fixed)
        || !ALIGNED(cacheddistance, fixed) || !ALIGNED(cachedxstep, fixed)
        || !ALIGNED(cachedystep, fixed))
        fail("unaligned renderer array");
#endif

    /* Repeat startup entry points after writing distinct data to all arrays:
     * they must preserve their permanent buffers and their accounting. */
    R_InitViewStorage();
    R_InitPlanes();
    if (allocations != 2 || R_StorageBytes() != total || region_count != 17)
        fail("renderer storage lifetime or accounting");
    for (i = 0; i < region_count; ++i)
        if (!cells(regions[i].data, regions[i].size, regions[i].value))
            fail("renderer arrays overlap or were reset");
    for (i = 0; i < 2; ++i) {
        if (!cells(blocks[i], GUARD_BYTES, 91)
            || !cells(payloads[i] + sizes[i], GUARD_BYTES, 91))
            fail("renderer slab guard overwritten");
        free(blocks[i]);
    }
    puts("RENDERER STORAGE OK grouped allocations, dirty zeroing, disjoint ranges, alignment and bank crossings");
    return 0;
}
