/* SPDX-License-Identifier: GPL-2.0-or-later
 * Relocate precomputed IWAD metadata without re-decoding octets or rehashing
 * names. The source image is prepared for Ripple's 16-bit C-byte ABI.
 */
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include "doom_build_config.h"
#include "asset_metadata.h"
#include "patch_cache.h"
#include "i_system.h"
#include "z_zone.h"

#if defined(RIPPLE) && DOOM_WAD_MODE == 2
/* The fixed-layout scan resolves the buffer bank once, then walks its cells.
 * Return low word: validation code; high word: absolute offending lump index.
 * This helper validates only; pointer relocation remains ordinary C below. */
extern uint32_t Doom_ValidateMetadata(const uint16_t *records,
                                     unsigned int count, unsigned int first,
                                     uint32_t file_length);

static void check_metadata_abi(void)
{
    /* Keep these runtime guards like the cooked patch guard: the compiler's
     * sizeof/offsetof conditional typedef-bound regression is registered in
     * c-test/tests/bugs/test_typedef_sizeof_conditional_bound.c. */
    if (sizeof(char) != 1 || sizeof(int) != 1 || sizeof(uint16_t) != 1
        || sizeof(uint32_t) != 2 || sizeof(void *) != 2
        || sizeof(lumpinfo_t) != 18
        || offsetof(lumpinfo_t, wad_file) != 8
        || offsetof(lumpinfo_t, position) != 10
        || offsetof(lumpinfo_t, size) != 12
        || offsetof(lumpinfo_t, cache) != 14
        || offsetof(lumpinfo_t, next) != 16)
        I_Error("Cooked metadata does not match the target C ABI");
}

static void check_metadata_range(uint32_t offset, uint32_t count)
{
    if (offset > DOOM_ASSET_TOTAL_WORDS
        || count > DOOM_ASSET_TOTAL_WORDS - offset)
        I_Error("Cooked metadata lies outside the asset image");
}

static void check_metadata_lumps(void)
{
    if (lumpinfo == NULL || numlumps == 0 || numlumps > 32767U
        || (uint32_t)numlumps != DOOM_ASSET_LUMP_COUNT)
        I_Error("Cooked metadata does not match the WAD directory");
}
#endif

int W_LoadAssetDirectory(wad_file_t *file, lumpinfo_t *dest, unsigned int count)
{
#if defined(RIPPLE) && DOOM_WAD_MODE == 2
    lumpinfo_t records[16];
    lumpinfo_t *output = dest;
    uint32_t offset = DOOM_ASSET_DIRECTORY_WORD;
    uint32_t file_length;
    unsigned int remaining = count;
    unsigned int index = 0;

    check_metadata_abi();
    if (file == NULL || dest == NULL || count == 0 || count > 32767U
        || (uint32_t)count != DOOM_ASSET_LUMP_COUNT)
        I_Error("Cooked directory does not match the WAD header");
    file_length = file->length;
    if (file_length != DOOM_WAD_LENGTH)
        I_Error("Cooked directory does not match the WAD length");
    check_metadata_range(offset, DOOM_ASSET_LUMP_COUNT * 18UL);

    while (remaining != 0) {
        unsigned int batch = remaining < 16U ? remaining : 16U;
        unsigned int left = batch;
        lumpinfo_t *record = records;
        unsigned int words = batch * 18U;
        uint32_t validation;
        W_ReadAssetWords(offset, (uint16_t *)records, (uint32_t)words);
        offset += words;
        validation = Doom_ValidateMetadata((uint16_t *)records, batch,
                                           index, file_length);
        if (validation != 0UL) {
            unsigned int bad_index = (unsigned int)(validation >> 16);
            unsigned int code = (unsigned int)validation;
            if (code == 1U)
                I_Error("Invalid cooked name at lump %u", bad_index);
            if (code == 2U)
                I_Error("Invalid cooked directory link at lump %u", bad_index);
            if (code == 3U)
                I_Error("Cooked lump %u lies outside the WAD", bad_index);
            I_Error("Invalid cooked metadata validation request");
        }
        while (left != 0) {
            unsigned int next = ((uint16_t *)record)[16];
            record->wad_file = file;
            /* Serialized zero placeholders must become canonical C NULL,
             * whose pointer bank is distinct from the bank-zero MMIO space. */
            record->cache = NULL;
            /* The batch validator already proved that the only out-of-range
             * link is the missing-link sentinel. Compare the small bound
             * instead of XORing each index with all sixteen bits set. */
            if (next < count) record->next = dest + next;
            else record->next = NULL;
            ++record;
            ++index;
            --left;
        }
        /* All validation and relocation used near stack storage. Publish a
         * contiguous batch through the port's bank-aware C-byte copy. */
        memcpy(output, records, (size_t)words);
        output += batch;
        remaining -= batch;
    }
    return 1;
#else
    (void)file;
    (void)dest;
    (void)count;
    return 0;
#endif
}

int W_LoadAssetHash(lumpinfo_t **heads)
{
#if defined(RIPPLE) && DOOM_WAD_MODE == 2
    uint16_t indices[32];
    lumpinfo_t *pointers[32];
    uint32_t offset = DOOM_ASSET_HASH_HEADS_WORD;
    unsigned int remaining;
    unsigned int limit;
    lumpinfo_t *base;

    check_metadata_abi();
    check_metadata_lumps();
    if (heads == NULL) I_Error("Null cooked hash destination");
    limit = numlumps;
    base = lumpinfo;
    remaining = limit;
    check_metadata_range(offset, DOOM_ASSET_LUMP_COUNT);
    while (remaining != 0) {
        unsigned int batch = remaining < 32U ? remaining : 32U;
        unsigned int left = batch;
        uint16_t *input = indices;
        lumpinfo_t **output = pointers;
        W_ReadAssetWords(offset, indices, (uint32_t)batch);
        offset += batch;
        while (left != 0) {
            unsigned int index = *input;
            if (index < limit) *output = base + index;
            else {
                if (index < UINT_MAX) I_Error("Invalid cooked hash head");
                *output = NULL;
            }
            ++input;
            ++output;
            --left;
        }
        memcpy(heads, pointers, (size_t)(batch * 2U));
        heads += batch;
        remaining -= batch;
    }
    return 1;
#else
    (void)heads;
    return 0;
#endif
}

int *W_LoadPatchNames(int pnames_lump, uint32_t *count)
{
#if defined(RIPPLE) && DOOM_WAD_MODE == 2
    int entries[32];
    int *lookup;
    int *output;
    uint32_t offset = DOOM_ASSET_PNAMES_WORD;
    uint32_t length;
    unsigned int remaining;
    unsigned int limit;

    check_metadata_abi();
    check_metadata_lumps();
    if (count == NULL || pnames_lump < 0
        || (unsigned int)pnames_lump >= numlumps
        || (uint32_t)pnames_lump != DOOM_ASSET_PNAMES_LUMP
        || DOOM_ASSET_PNAMES_COUNT > 32767UL)
        I_Error("Cooked patch names do not match PNAMES");
    length = W_LumpLength((unsigned int)pnames_lump);
    if (length < 4UL || DOOM_ASSET_PNAMES_COUNT * 8UL > length - 4UL)
        I_Error("Cooked patch names exceed the PNAMES lump");
    check_metadata_range(offset, DOOM_ASSET_PNAMES_COUNT);
    limit = numlumps;
    remaining = (unsigned int)DOOM_ASSET_PNAMES_COUNT;
    /* An empty but valid lookup must still return non-NULL: NULL selects the
     * raw decoder in the caller. Its unused storage is never inspected. */
    lookup = Z_Malloc((size_t)(remaining ? remaining : 1U) * sizeof(*lookup),
                      PU_STATIC, NULL);
    if (lookup == NULL) I_Error("Cannot allocate cooked patch names");
    output = lookup;
    while (remaining != 0) {
        unsigned int batch = remaining < 32U ? remaining : 32U;
        unsigned int left = batch;
        int *entry = entries;
        W_ReadAssetWords(offset, (uint16_t *)entries, (uint32_t)batch);
        offset += batch;
        while (left != 0) {
            unsigned int index = (unsigned int)*entry;
            if (index >= limit) {
                if (index < UINT_MAX) I_Error("Invalid cooked PNAMES patch ID");
                *entry = -1;
            }
            ++entry;
            --left;
        }
        memcpy(output, entries, (size_t)batch);
        output += batch;
        remaining -= batch;
    }
    *count = DOOM_ASSET_PNAMES_COUNT;
    return lookup;
#else
    (void)pnames_lump;
    (void)count;
    return NULL;
#endif
}
