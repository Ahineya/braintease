#include "wad_io.h"
#include "doom_build_config.h"
#include <stdlib.h>

#if defined(RIPPLE) && !defined(DOOM_GUEST)
#include <mmio.h>
/* The leaf copies an already selected disk span and returns the advanced
 * canonical destination. It retains the cursor across all words in registers. */
extern uint16_t *Doom_ReadStorageWords(uint16_t *dest, uint16_t count);
#else
#include <stdio.h>
#endif

static int wad_fail(WadFile *wad, const char *error) {
    wad->error = error;
    return 0;
}

int wad_range(uint32_t length, uint32_t offset, uint32_t count) {
    return offset <= length && count <= length - offset;
}

int wad_u16(const WadOctet *bytes, uint32_t length, uint32_t offset, uint16_t *out) {
    uint16_t lo;
    uint16_t hi;
    if (!wad_range(length, offset, 2UL)) return 0;
    bytes += offset;
    lo = (uint16_t)*bytes++ & 255U;
    hi = (uint16_t)*bytes & 255U;
    /* Octet/word fields are disjoint, so addition cannot carry between them. */
    *out = lo + (hi << 8);
    return 1;
}

int wad_u32(const WadOctet *bytes, uint32_t length, uint32_t offset, uint32_t *out) {
    uint16_t lo;
    uint16_t hi;
    if (!wad_range(length, offset, 4UL)) return 0;
    /* Check the complete field once, then walk its four cells. This avoids
     * nested calls and repeated 32-bit indexing on the 16-bit guest. */
    bytes += offset;
    lo = (uint16_t)*bytes++ & 255U;
    lo += ((uint16_t)*bytes++ & 255U) << 8;
    hi = (uint16_t)*bytes++ & 255U;
    hi += ((uint16_t)*bytes & 255U) << 8;
    *out = (uint32_t)lo + ((uint32_t)hi << 16);
    return 1;
}

int wad_read_words(WadFile *wad, uint32_t offset, uint16_t *dest, uint32_t count) {
#if DOOM_WAD_MODE != 0
    if (!wad_range(DOOM_ASSET_TOTAL_WORDS, offset, count))
        return wad_fail(wad, "read outside asset image");
    if (count == 0) return 1;
    if (!dest) return wad_fail(wad, "null asset destination");
#if defined(RIPPLE) && !defined(DOOM_GUEST)
    /* Disk addresses count words. Split at each 32,768-word block boundary;
     * the leaf handles RAM boundaries without any per-word bit operations. */
    while (count != 0) {
        uint16_t within = (uint16_t)offset;
        uint16_t block = (uint16_t)(offset >> 16);
        uint16_t chunk;
        /* Split the low word without a mask or a cross-word shift. The asset
         * range bounds a nonempty read below the 2^31-word storage limit. */
        block += block;
        if (within >= 32768U) {
            within -= 32768U;
            ++block;
        }
        chunk = 32768U - within;
        if ((uint32_t)chunk > count) chunk = (uint16_t)count;
        mmio_write(MMIO_STORE_BLOCK, block);
        mmio_write(MMIO_STORE_ADDR, within);
        dest = Doom_ReadStorageWords(dest, chunk);
        offset += chunk;
        count -= chunk;
    }
#else
    if (!wad->asset_file) {
        wad->asset_file = fopen(DOOM_ASSET_PATH, "rb");
        if (!wad->asset_file) return wad_fail(wad, "cannot open asset image");
    }
    /* Host C bytes are octets; do not cast serialized words to host structs. */
    if (fseek((FILE *)wad->asset_file, (long)((unsigned long)offset * 2UL), SEEK_SET))
        return wad_fail(wad, "asset seek failed");
    while (count != 0) {
        int lo = fgetc((FILE *)wad->asset_file);
        int hi = fgetc((FILE *)wad->asset_file);
        if (lo == EOF || hi == EOF) return wad_fail(wad, "short asset read");
        *dest++ = (uint16_t)((unsigned int)lo + ((unsigned int)hi << 8));
        --count;
    }
#endif
    return 1;
#else
    return wad_fail(wad, "word reads require expanded asset storage");
#endif
}

int wad_read(WadFile *wad, uint32_t offset, WadOctet *dest, uint32_t count) {
    if (!wad_range(wad->length, offset, count)) return wad_fail(wad, "read outside WAD");
    if (count == 0) return 1;
    if (!dest) return wad_fail(wad, "null read destination");
#if defined(RIPPLE) && !defined(DOOM_GUEST)
#if DOOM_WAD_MODE != 0
    /* Each serialized octet already occupies a complete disk word, exactly
     * matching a Ripple C byte. Native validation still reads the source WAD. */
    return wad_read_words(wad, offset, (uint16_t *)dest, count);
#else
    /* A storage block contains 65,536 octets / 32,768 little-endian words.
     * MMIO's address register selects a word; it auto-increments per read.
     * Reprogram at every block boundary rather than relying on wrap behavior. */
    while (count != 0) {
        uint16_t within = (uint16_t)offset;
        uint16_t chunk = 32768U;
        uint16_t word;
        /* Bound the inner loop to one 16-bit counter and one disk block.
         * Direct MMIO keeps the cursor and destination in registers instead
         * of spilling them around a function call for every packed word. */
        if (within > 32768U) chunk = (uint16_t)(0U - within);
        if ((uint32_t)chunk > count) chunk = (uint16_t)count;
        mmio_write(MMIO_STORE_BLOCK, (uint16_t)(offset >> 16));
        mmio_write(MMIO_STORE_ADDR, within >> 1);
        offset += chunk;
        count -= chunk;
        if (within & 1U) {
            word = mmio_read(MMIO_STORE_DATA);
            *dest++ = (WadOctet)(word >> 8);
            --chunk;
        }
        while (chunk >= 2U) {
            word = mmio_read(MMIO_STORE_DATA);
            *dest++ = (WadOctet)(word & 255U);
            *dest++ = (WadOctet)(word >> 8);
            chunk -= 2U;
        }
        if (chunk != 0) *dest++ = (WadOctet)(mmio_read(MMIO_STORE_DATA) & 255U);
    }
#endif
#else
    if (!wad->host_file) return wad_fail(wad, "WAD is not open");
    if (fseek((FILE *)wad->host_file, (long)offset, SEEK_SET) != 0)
        return wad_fail(wad, "WAD seek failed");
    if (fread(dest, 1, (size_t)count, (FILE *)wad->host_file) != (size_t)count)
        return wad_fail(wad, "short WAD read");
#endif
    return 1;
}

int wad_read_lump(WadFile *wad, const WadLump *lump, uint32_t offset,
                  WadOctet *dest, uint32_t count) {
    if (!wad_range(wad->length, lump->offset, lump->length) ||
        !wad_range(lump->length, offset, count)) return wad_fail(wad, "read outside lump");
    return wad_read(wad, lump->offset + offset, dest, count);
}

void wad_close(WadFile *wad) {
    if (wad->lumps) free(wad->lumps);
    wad->lumps = NULL;
#if !defined(RIPPLE) || defined(DOOM_GUEST)
    if (wad->host_file) fclose((FILE *)wad->host_file);
    if (wad->asset_file) fclose((FILE *)wad->asset_file);
#endif
    wad->host_file = NULL;
    wad->asset_file = NULL;
    wad->length = 0;
    wad->lump_count = 0;
    wad->directory_offset = 0;
}

int wad_open_stream(WadFile *wad, const char *path, uint32_t expected_octet_length) {
    wad->host_file = NULL;
    wad->asset_file = NULL;
    wad->lumps = NULL;
    wad->length = expected_octet_length;
    wad->lump_count = 0;
    wad->directory_offset = 0;
    wad->error = NULL;
    if (expected_octet_length < 12UL) return wad_fail(wad, "WAD header is truncated");
#if !defined(RIPPLE) || defined(DOOM_GUEST)
    {
        FILE *file = fopen(path, "rb");
        long length;
        if (!file) return wad_fail(wad, "cannot open WAD");
        wad->host_file = file;
        if (fseek(file, 0, SEEK_END) != 0) {
            wad_close(wad);
            return wad_fail(wad, "cannot measure WAD");
        }
        length = ftell(file);
        if (length < 0 || (unsigned long)length != (unsigned long)expected_octet_length) {
            wad_close(wad);
            return wad_fail(wad, "WAD length differs from fixture manifest");
        }
    }
#endif
#if defined(RIPPLE) && DOOM_WAD_MODE == 2
    {
        uint16_t header[44];
        if (expected_octet_length != DOOM_WAD_LENGTH)
            return wad_fail(wad, "asset source length mismatch");
        if (!wad_read_words(wad, DOOM_ASSET_HEADER_WORD, header, 44UL)) return 0;
        if (header[0] != 0x5257U || header[1] != 0x4144U
            || header[2] != 5U || header[3] != 1U
            || header[4] != (uint16_t)DOOM_WAD_LENGTH
            || header[5] != (uint16_t)(DOOM_WAD_LENGTH >> 16)
            || header[6] != (uint16_t)DOOM_ASSET_LUMP_COUNT
            || header[7] != (uint16_t)(DOOM_ASSET_LUMP_COUNT >> 16)
            || header[8] != (uint16_t)DOOM_ASSET_INDEX_WORD
            || header[9] != (uint16_t)(DOOM_ASSET_INDEX_WORD >> 16)
            || header[10] != (uint16_t)DOOM_ASSET_TOTAL_WORDS
            || header[11] != (uint16_t)(DOOM_ASSET_TOTAL_WORDS >> 16)
            || header[12] != DOOM_ASSET_FINGERPRINT_LO
            || header[13] != DOOM_ASSET_FINGERPRINT_HI
            || header[14] != (uint16_t)DOOM_ASSET_DIRECTORY_WORD
            || header[15] != (uint16_t)(DOOM_ASSET_DIRECTORY_WORD >> 16)
            || header[16] != (uint16_t)DOOM_ASSET_HASH_HEADS_WORD
            || header[17] != (uint16_t)(DOOM_ASSET_HASH_HEADS_WORD >> 16)
            || header[18] != (uint16_t)DOOM_ASSET_PNAMES_WORD
            || header[19] != (uint16_t)(DOOM_ASSET_PNAMES_WORD >> 16)
            || header[20] != (uint16_t)DOOM_ASSET_PNAMES_COUNT
            || header[21] != (uint16_t)(DOOM_ASSET_PNAMES_COUNT >> 16)
            || header[22] != (uint16_t)DOOM_ASSET_PNAMES_LUMP
            || header[23] != (uint16_t)(DOOM_ASSET_PNAMES_LUMP >> 16)
            || header[24] != 1U || header[25] != 18U
            || header[26] != 1U || header[27] != 1U
            || header[28] != (uint16_t)DOOM_ASSET_TEXTURE_WORD
            || header[29] != (uint16_t)(DOOM_ASSET_TEXTURE_WORD >> 16)
            || header[30] != (uint16_t)DOOM_ASSET_TEXTURE_WORDS
            || header[31] != (uint16_t)(DOOM_ASSET_TEXTURE_WORDS >> 16)
            || header[32] != (uint16_t)DOOM_ASSET_SPRITE_WORD
            || header[33] != (uint16_t)(DOOM_ASSET_SPRITE_WORD >> 16)
            || header[34] != (uint16_t)DOOM_ASSET_SPRITE_WORDS
            || header[35] != (uint16_t)(DOOM_ASSET_SPRITE_WORDS >> 16)
            || header[36] != (uint16_t)DOOM_ASSET_RENDERER_WORD
            || header[37] != (uint16_t)(DOOM_ASSET_RENDERER_WORD >> 16)
            || header[38] != (uint16_t)DOOM_ASSET_RENDERER_WORDS
            || header[39] != (uint16_t)(DOOM_ASSET_RENDERER_WORDS >> 16)
            || header[40] != (uint16_t)DOOM_ASSET_SPRITE_METRICS_WORD
            || header[41] != (uint16_t)(DOOM_ASSET_SPRITE_METRICS_WORD >> 16)
            || header[42] != (uint16_t)DOOM_ASSET_SPRITE_METRICS_WORDS
            || header[43] != (uint16_t)(DOOM_ASSET_SPRITE_METRICS_WORDS >> 16)) {
            wad_close(wad);
            return wad_fail(wad, "cooked asset version or source mismatch");
        }
        /* The compiled manifest fixes the complete non-overlapping layout.
         * Check spans before any metadata can become guest pointers. */
        if (DOOM_ASSET_LUMP_COUNT == 0UL || DOOM_ASSET_LUMP_COUNT > 32767UL
            || DOOM_ASSET_PNAMES_COUNT > 32767UL
            || DOOM_ASSET_INDEX_WORD != DOOM_ASSET_HEADER_WORD + 44UL
            || DOOM_ASSET_DIRECTORY_WORD != DOOM_ASSET_INDEX_WORD + DOOM_ASSET_LUMP_COUNT * 4UL
            || DOOM_ASSET_HASH_HEADS_WORD != DOOM_ASSET_DIRECTORY_WORD + DOOM_ASSET_LUMP_COUNT * 18UL
            || DOOM_ASSET_PNAMES_WORD != DOOM_ASSET_HASH_HEADS_WORD + DOOM_ASSET_LUMP_COUNT
            || !wad_range(DOOM_ASSET_TOTAL_WORDS, DOOM_ASSET_PNAMES_WORD, DOOM_ASSET_PNAMES_COUNT)
            || (DOOM_ASSET_PNAMES_LUMP != UINT32_MAX && DOOM_ASSET_PNAMES_LUMP >= DOOM_ASSET_LUMP_COUNT)
            || (DOOM_ASSET_PNAMES_LUMP == UINT32_MAX && DOOM_ASSET_PNAMES_COUNT != 0UL)
            || (DOOM_ASSET_TEXTURE_WORDS == 0UL
                && (DOOM_ASSET_TEXTURE_WORD != 0UL || DOOM_ASSET_TEXTURE_COUNT != 0UL
                    || DOOM_ASSET_TEXTURE_COLUMNS != 0UL || DOOM_ASSET_TEXTURE_DEFINITION_WORDS != 0UL))
            || (DOOM_ASSET_TEXTURE_WORDS != 0UL
                && (DOOM_ASSET_TEXTURE_WORD != DOOM_ASSET_PNAMES_WORD + DOOM_ASSET_PNAMES_COUNT
                    || DOOM_ASSET_TEXTURE_COUNT == 0UL || DOOM_ASSET_TEXTURE_COUNT > 32767UL
                    || DOOM_ASSET_TEXTURE_WORDS < 16UL
                    || !wad_range(DOOM_ASSET_TOTAL_WORDS, DOOM_ASSET_TEXTURE_WORD, DOOM_ASSET_TEXTURE_WORDS)))
            || (DOOM_ASSET_SPRITE_WORDS == 0UL && DOOM_ASSET_SPRITE_WORD != 0UL)
            || (DOOM_ASSET_SPRITE_WORDS != 0UL
                && (DOOM_ASSET_SPRITE_WORD != DOOM_ASSET_PNAMES_WORD + DOOM_ASSET_PNAMES_COUNT + DOOM_ASSET_TEXTURE_WORDS
                    || !wad_range(DOOM_ASSET_TOTAL_WORDS, DOOM_ASSET_SPRITE_WORD, DOOM_ASSET_SPRITE_WORDS)))
            || (DOOM_ASSET_SPRITE_METRICS_WORDS == 0UL && DOOM_ASSET_SPRITE_METRICS_WORD != 0UL)
            || (DOOM_ASSET_SPRITE_METRICS_WORDS != 0UL
                && (DOOM_ASSET_SPRITE_METRICS_WORD != DOOM_ASSET_PNAMES_WORD + DOOM_ASSET_PNAMES_COUNT
                                                    + DOOM_ASSET_TEXTURE_WORDS + DOOM_ASSET_SPRITE_WORDS
                    || DOOM_ASSET_SPRITE_METRICS_WORDS < 16UL
                    || !wad_range(DOOM_ASSET_TOTAL_WORDS, DOOM_ASSET_SPRITE_METRICS_WORD, DOOM_ASSET_SPRITE_METRICS_WORDS)))
            || DOOM_ASSET_RENDERER_WORD != DOOM_ASSET_PNAMES_WORD + DOOM_ASSET_PNAMES_COUNT
                                           + DOOM_ASSET_TEXTURE_WORDS + DOOM_ASSET_SPRITE_WORDS
                                           + DOOM_ASSET_SPRITE_METRICS_WORDS
            || DOOM_ASSET_RENDERER_WORDS < 16UL
            || !wad_range(DOOM_ASSET_TOTAL_WORDS, DOOM_ASSET_RENDERER_WORD, DOOM_ASSET_RENDERER_WORDS)) {
            wad_close(wad);
            return wad_fail(wad, "invalid cooked metadata layout");
        }
    }
#endif
    return 1;
}

int wad_open(WadFile *wad, const char *path, uint32_t expected_octet_length) {
    WadOctet header[12];
    WadOctet record[16];
    WadLump *lump;
    uint32_t count;
    uint32_t directory;
    uint32_t pos;
    uint32_t i;
    uint16_t j;
    if (!wad_open_stream(wad, path, expected_octet_length)) return 0;
    if (!wad_read(wad, 0UL, header, 12UL)) { wad_close(wad); return 0; }
    if (!((header[0] == 'I' || header[0] == 'P') && header[1] == 'W' &&
          header[2] == 'A' && header[3] == 'D')) {
        wad_close(wad);
        return wad_fail(wad, "invalid WAD signature");
    }
    wad_u32(header, 12UL, 4UL, &count);
    wad_u32(header, 12UL, 8UL, &directory);
    if (count == 0 || count > expected_octet_length / 16UL ||
        !wad_range(expected_octet_length, directory, count * 16UL) ||
        (size_t)count > SIZE_MAX / sizeof(WadLump)) {
        wad_close(wad);
        return wad_fail(wad, "invalid WAD directory");
    }
    wad->lumps = malloc((size_t)count * sizeof(WadLump));
    if (!wad->lumps) { wad_close(wad); return wad_fail(wad, "no memory for WAD directory"); }
    wad->lump_count = count;
    wad->directory_offset = directory;
    lump = wad->lumps;
    pos = directory;
    for (i = 0; i < count; ++i) {
        if (!wad_read(wad, pos, record, 16UL)) { wad_close(wad); return 0; }
        wad_u32(record, 16UL, 0UL, &lump->offset);
        wad_u32(record, 16UL, 4UL, &lump->length);
        if (!wad_range(expected_octet_length, lump->offset, lump->length)) {
            wad_close(wad);
            return wad_fail(wad, "lump outside WAD");
        }
        for (j = 0; j < 8; ++j) lump->name[j] = (char)record[8 + j];
        lump->name[8] = 0;
        ++lump;
        pos += 16UL;
    }
    return 1;
}

int wad_lump_at(WadFile *wad, uint32_t index, WadLump *lump) {
    if (index >= wad->lump_count) return wad_fail(wad, "lump index outside directory");
    *lump = wad->lumps[index];
    return 1;
}

static char upper_ascii(char c) {
    return c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c;
}

int wad_find(WadFile *wad, const char *name, WadLump *lump) {
    uint16_t length = 0;
    uint32_t index = wad->lump_count;
    while (length < 9 && name[length]) ++length;
    if (length > 8) return wad_fail(wad, "lump name exceeds eight characters");
    while (index != 0) {
        WadLump *candidate;
        uint16_t i;
        int equal = 1;
        --index;
        candidate = &wad->lumps[index];
        for (i = 0; i < 8; ++i) {
            char wanted = i < length ? upper_ascii(name[i]) : 0;
            if (upper_ascii(candidate->name[i]) != wanted) equal = 0;
        }
        if (equal) { *lump = *candidate; return 1; }
    }
    return wad_fail(wad, "lump not found");
}

int wad_decode_patch(const WadOctet *bytes, uint32_t length,
                     WadOctet *indexed, uint16_t width, uint16_t height) {
    uint16_t actual_width;
    uint16_t actual_height;
    uint16_t x;
    uint32_t size = (uint32_t)width * (uint32_t)height;
    uint32_t i;
    WadOctet *clear = indexed;
    if (!indexed || width == 0 || height == 0 || length < 8UL) return 0;
    if (!wad_u16(bytes, length, 0UL, &actual_width) ||
        !wad_u16(bytes, length, 2UL, &actual_height)) return 0;
    if (actual_width != width || actual_height != height ||
        !wad_range(length, 8UL, (uint32_t)width * 4UL)) return 0;
    for (i = 0; i < size; ++i) *clear++ = 0;
    for (x = 0; x < width; ++x) {
        uint32_t offset;
        int terminated = 0;
        if (!wad_u32(bytes, length, 8UL + (uint32_t)x * 4UL, &offset)) return 0;
        if (offset < 8UL + (uint32_t)width * 4UL) return 0;
        while (offset < length) {
            uint16_t top = bytes[offset] & 255U;
            uint16_t run;
            uint16_t y;
            if (top == 255U) { terminated = 1; break; }
            if (!wad_range(length, offset, 3UL)) return 0;
            run = bytes[offset + 1UL] & 255U;
            if ((uint32_t)top + run > height ||
                !wad_range(length, offset, (uint32_t)run + 4UL)) return 0;
            for (y = 0; y < run; ++y) {
                indexed[((uint32_t)top + y) * width + x] = bytes[offset + 3UL + y] & 255U;
            }
            offset += (uint32_t)run + 4UL;
        }
        if (!terminated) return 0;
    }
    return 1;
}

uint16_t wad_rgb565(WadOctet red, WadOctet green, WadOctet blue) {
    return (((uint16_t)red & 248U) << 8) +
           (((uint16_t)green & 252U) << 3) +
           (((uint16_t)blue & 255U) >> 3);
}
