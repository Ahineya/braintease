#include "wad_io.h"
#include <stdlib.h>

#ifdef RIPPLE
#include <mmio.h>
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
    lo = (uint16_t)bytes[offset] & 255U;
    hi = (uint16_t)bytes[offset + 1UL] & 255U;
    *out = lo | (hi << 8);
    return 1;
}

int wad_u32(const WadOctet *bytes, uint32_t length, uint32_t offset, uint32_t *out) {
    uint16_t lo;
    uint16_t hi;
    if (!wad_range(length, offset, 4UL)) return 0;
    if (!wad_u16(bytes, length, offset, &lo)) return 0;
    if (!wad_u16(bytes, length, offset + 2UL, &hi)) return 0;
    *out = (uint32_t)lo | ((uint32_t)hi << 16);
    return 1;
}

int wad_read(WadFile *wad, uint32_t offset, WadOctet *dest, uint32_t count) {
    if (!wad_range(wad->length, offset, count)) return wad_fail(wad, "read outside WAD");
    if (count == 0) return 1;
    if (!dest) return wad_fail(wad, "null read destination");
#ifdef RIPPLE
    /* A storage block contains 65,536 octets / 32,768 little-endian words.
     * MMIO's address register selects a word; it auto-increments per read.
     * Reprogram at every block boundary rather than relying on wrap behavior. */
    while (count != 0) {
        uint16_t within = (uint16_t)offset;
        uint32_t chunk = 65536UL - (uint32_t)within;
        uint16_t word;
        if (chunk > count) chunk = count;
        storage_set_block((uint16_t)(offset >> 16));
        storage_set_addr(within >> 1);
        offset += chunk;
        count -= chunk;
        if (within & 1U) {
            word = storage_read();
            *dest++ = (WadOctet)((word >> 8) & 255U);
            --chunk;
        }
        while (chunk >= 2UL) {
            word = storage_read();
            *dest++ = (WadOctet)(word & 255U);
            *dest++ = (WadOctet)((word >> 8) & 255U);
            chunk -= 2UL;
        }
        if (chunk != 0) *dest++ = (WadOctet)(storage_read() & 255U);
    }
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
#ifndef RIPPLE
    if (wad->host_file) fclose((FILE *)wad->host_file);
#endif
    wad->host_file = NULL;
    wad->length = 0;
    wad->lump_count = 0;
    wad->directory_offset = 0;
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
    wad->host_file = NULL;
    wad->lumps = NULL;
    wad->length = expected_octet_length;
    wad->lump_count = 0;
    wad->directory_offset = 0;
    wad->error = NULL;
    if (expected_octet_length < 12UL) return wad_fail(wad, "WAD header is truncated");
#ifndef RIPPLE
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
    return (((uint16_t)red & 248U) << 8) |
           (((uint16_t)green & 252U) << 3) |
           (((uint16_t)blue & 255U) >> 3);
}
