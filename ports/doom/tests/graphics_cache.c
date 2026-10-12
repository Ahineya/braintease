/* Shared native/Ripple fixture for the decoded engine patch cache.
 * Sources: this file, src/patch_cache.c, src/wad_io.c. It supplies WAD and
 * zone services; no filesystem, display, real clock or Brainfuck is needed.
 * Headers come from the prepared engine and src; use DOOM_PORT=1.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "patch_cache.h"
#include "w_wad.h"
#include "z_zone.h"

unsigned int numlumps = 3;
static byte *raw_lumps[3];
static uint32_t raw_lengths[3];
static int raw_tags[3];
static int reads[3];
static int releases[3];

typedef struct Allocation {
    void *memory;
    void **owner;
    size_t size;
    int tag;
    struct Allocation *next;
} Allocation;

static Allocation *allocations;

static void check(const char *name, int good)
{
    if (!good) {
        printf("GRAPHICS FAIL %s\n", name);
        exit(1);
    }
}

void I_Error(char *format, ...)
{
    printf("GRAPHICS ERROR %s\n", format);
    exit(1);
}

static Allocation *find_allocation(void *memory)
{
    Allocation *entry = allocations;
    while (entry != NULL && entry->memory != memory) entry = entry->next;
    check("zone pointer is registered", entry != NULL);
    return entry;
}

void *Z_Malloc(size_t size, int tag, void *owner)
{
    Allocation *entry = malloc(sizeof(*entry));
    check("zone metadata allocation", entry != NULL);
    entry->memory = calloc(size ? size : 1, 1);
    check("zone payload allocation", entry->memory != NULL);
    entry->owner = owner;
    entry->size = size;
    entry->tag = tag;
    entry->next = allocations;
    allocations = entry;
    if (entry->owner != NULL) *entry->owner = entry->memory;
    return entry->memory;
}

void Z_Free(void *memory)
{
    Allocation *entry = allocations;
    Allocation *previous = NULL;
    while (entry != NULL && entry->memory != memory) {
        previous = entry;
        entry = entry->next;
    }
    check("zone free is registered", entry != NULL);
    if (entry->owner != NULL) *entry->owner = NULL;
    if (previous != NULL) previous->next = entry->next;
    else allocations = entry->next;
    free(entry->memory);
    free(entry);
}

void Z_ChangeTag2(void *memory, int tag, char *file, int line)
{
    (void) file;
    (void) line;
    find_allocation(memory)->tag = tag;
}

uint32_t W_LumpLength(unsigned int lump)
{
    check("lump length index", lump < numlumps);
    return raw_lengths[lump];
}

void *W_CacheLumpNum(int lump, int tag)
{
    check("raw cache index", lump >= 0 && (unsigned int) lump < numlumps);
    raw_tags[lump] = tag;
    ++reads[lump];
    return raw_lumps[lump];
}

void W_ReleaseLumpNum(int lump)
{
    check("raw data pinned through decode", raw_tags[lump] == PU_STATIC);
    raw_tags[lump] = PU_CACHE;
    ++releases[lump];
}

int W_GetNumForName(char *name)
{
    check("patch lookup name", strcmp(name, "NINE") == 0);
    return 1;
}

static void put16(byte *raw, size_t at, uint16_t value)
{
    raw[at] = (byte) (value & 255U);
    raw[at + 1] = (byte) (value >> 8);
}

static void put32(byte *raw, size_t at, uint32_t value)
{
    put16(raw, at, (uint16_t) value);
    put16(raw, at + 2, (uint16_t) (value >> 16));
}

static void make_patch(int lump, uint16_t width, uint32_t first_column)
{
    uint16_t x;
    uint32_t at;
    raw_lengths[lump] = first_column + (uint32_t) width * 6UL;
    raw_lumps[lump] = calloc((size_t) raw_lengths[lump], 1);
    check("raw patch allocation", raw_lumps[lump] != NULL);
    put16(raw_lumps[lump], 0, width);
    put16(raw_lumps[lump], 2, 1);
    put16(raw_lumps[lump], 4, UINT16_C(65531)); /* signed -5 */
    put16(raw_lumps[lump], 6, 7);
    for (x = 0; x < width; ++x) {
        at = first_column + (uint32_t) x * 6UL;
        put32(raw_lumps[lump], 8UL + (uint32_t) x * 4UL, at);
        raw_lumps[lump][at] = 0;
        raw_lumps[lump][at + 1] = 1;
        raw_lumps[lump][at + 2] = 0;
        raw_lumps[lump][at + 3] = (byte) (30 + x);
        raw_lumps[lump][at + 4] = 0;
        raw_lumps[lump][at + 5] = 255;
    }
}

static void check_patch(int lump, patch_t *patch, uint16_t width, uint32_t raw_column)
{
    uint16_t x;
    size_t header = offsetof(patch_t, columnofs)
                  + (size_t) (width < 8 ? 8 : width) * sizeof(patch->columnofs[0]);
    size_t directory = 8UL + (size_t) width * 4UL;
    size_t expected_size = header + (size_t) raw_lengths[lump] - directory;
    check("decoded dimensions", patch->width == width && patch->height == 1);
    check("signed offsets", patch->leftoffset == -5 && patch->topoffset == 7);
    check("decoded allocation units", find_allocation(patch)->size == expected_size);
    for (x = 0; x < width; ++x) {
        const byte *column = (const byte *) patch + patch->columnofs[x];
        check("column offset in C units", patch->columnofs[x]
              == (int32_t) (header + raw_column + (uint32_t) x * 6UL - directory));
        check("post layout", column[0] == 0 && column[1] == 1 && column[2] == 0
              && column[3] == 30 + x && column[4] == 0 && column[5] == 255);
    }
}

static void check_malformed(void)
{
    byte raw[18] = {1,0,1,0,0,0,0,0,12,0,0,0,0,1,0,7,0,255};
    check("valid patch validation", Doom_ValidatePatch(raw, 18) == NULL);
    check("truncated fixed header", Doom_ValidatePatch(raw, 7) != NULL);
    check("truncated offset table", Doom_ValidatePatch(raw, 11) != NULL);
    check("missing column terminator", Doom_ValidatePatch(raw, 17) != NULL);
    raw[0] = 0;
    check("zero width", Doom_ValidatePatch(raw, 18) != NULL);
    raw[0] = 1;
    raw[3] = 128;
    check("out-of-range signed height", Doom_ValidatePatch(raw, 18) != NULL);
    raw[3] = 0;
    raw[8] = 8;
    check("column inside header", Doom_ValidatePatch(raw, 18) != NULL);
    put32(raw, 8, UINT32_MAX);
    check("column past end", Doom_ValidatePatch(raw, 18) != NULL);
    put32(raw, 8, 12);
    raw[12] = 1;
    check("post past image bottom", Doom_ValidatePatch(raw, 18) != NULL);
    raw[12] = 0;
    raw[13] = 255;
    check("truncated post payload", Doom_ValidatePatch(raw, 18) != NULL);
    raw[13] = 1;
    raw[17] = 0;
    check("truncated next post", Doom_ValidatePatch(raw, 18) != NULL);
    /* These lengths reject before any attempted access to their payload. */
    check("null raw pointer", Doom_ValidatePatch(NULL, 18) != NULL);
    check("oversized decoded payload", Doom_ValidatePatch(raw, UINT32_MAX) != NULL);
}

int main(void)
{
    patch_t *one;
    patch_t *nine;
    patch_t *wide;
    unsigned int lump;
    check_malformed();
    make_patch(0, 1, 12);
    make_patch(1, 9, 44);
    make_patch(2, 1, 70000UL);
    W_ReleasePatchNum(0);
    W_ReleasePatchName("NINE");
    check("uncached release does not read raw data", reads[0] == 0 && reads[1] == 0);
    one = W_CachePatchNum(0, PU_CACHE);
    check_patch(0, one, 1, 12);
    check("cache keeps persistent owner", find_allocation(one)->owner != NULL);
    check("raw released after decode", releases[0] == 1 && raw_tags[0] == PU_CACHE);
    check("cache hit reuses decoded storage", W_CachePatchNum(0, PU_STATIC) == one);
    check("cache hit retags", find_allocation(one)->tag == PU_STATIC && reads[0] == 1);
    raw_tags[0] = -1; /* The raw cache may be purged independently. */
    W_ReleasePatchNum(0);
    check("release retags decoded storage", find_allocation(one)->tag == PU_CACHE);
    check("release never touches raw storage", reads[0] == 1 && releases[0] == 1 && raw_tags[0] == -1);
    Z_Free(one); /* Mimic a zone purge, which must clear the owner's slot. */
    W_ReleasePatchNum(0); /* An already-purged decoded entry is a no-op. */
    one = W_CachePatchNum(0, PU_CACHE);
    check("purge reloads raw source", reads[0] == 2 && releases[0] == 2);
    check_patch(0, one, 1, 12);
    nine = W_CachePatchName("NINE", PU_STATIC);
    check_patch(1, nine, 9, 44);
    W_ReleasePatchName("NINE");
    check("named release retags decoded storage", find_allocation(nine)->tag == PU_CACHE && releases[1] == 1);
    wide = W_CachePatchNum(2, PU_CACHE);
    check_patch(2, wide, 1, 70000UL);
    check("large column offset preserved", wide->columnofs[0] > INT32_C(65535));
    while (allocations != NULL) Z_Free(allocations->memory);
    W_ReleasePatchName("NINE");
    for (lump = 0; lump < numlumps; ++lump) free(raw_lumps[lump]);
    puts("GRAPHICS OK decoded widths 1/9, bank-crossing offsets, cache purge/reload, 12 malformed cases");
    return 0;
}
