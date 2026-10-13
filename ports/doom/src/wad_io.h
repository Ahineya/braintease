#ifndef RIPPLE_DOOM_WAD_IO_H
#define RIPPLE_DOOM_WAD_IO_H

#include <stdint.h>
#include <stddef.h>

/* One serialized octet per C element. Ripple elements are 16-bit cells;
 * serialized offsets/lengths always count eight-bit file octets. */
typedef unsigned char WadOctet;

typedef struct WadLump {
    uint32_t offset;
    uint32_t length;
    char name[9];
} WadLump;

typedef struct WadFile {
    void *host_file;
    void *asset_file;
    uint32_t length;
    uint32_t directory_offset;
    uint32_t lump_count;
    WadLump *lumps;
    const char *error;
} WadFile;

/* Returns 1 on success, 0 on failure. The supplied length is mandatory: the
 * storage MMIO has no file-size query. Host builds also verify this length. */
int wad_open(WadFile *wad, const char *path, uint32_t expected_octet_length);
/* Open only the bounded byte stream, without reading or indexing its directory.
 * The caller must validate the WAD header and entries (Doom's W_AddFile does).
 * Use wad_open instead when wad_find/wad_lump_at are needed. */
int wad_open_stream(WadFile *wad, const char *path, uint32_t expected_octet_length);
void wad_close(WadFile *wad);
int wad_read(WadFile *wad, uint32_t offset, WadOctet *dest, uint32_t count);
/* Read physical 16-bit words from the configured expanded/cooked image. Word
 * offsets are deliberately distinct from logical WAD octet offsets. */
int wad_read_words(WadFile *wad, uint32_t word_offset, uint16_t *dest, uint32_t count);
int wad_read_lump(WadFile *wad, const WadLump *lump, uint32_t offset,
                  WadOctet *dest, uint32_t count);
/* Last matching name wins, as in Doom. Names are at most eight ASCII chars. */
int wad_find(WadFile *wad, const char *name, WadLump *lump);
int wad_lump_at(WadFile *wad, uint32_t index, WadLump *lump);
int wad_range(uint32_t length, uint32_t offset, uint32_t count);
int wad_u16(const WadOctet *bytes, uint32_t length, uint32_t offset, uint16_t *out);
int wad_u32(const WadOctet *bytes, uint32_t length, uint32_t offset, uint32_t *out);

/* Decode a vanilla Doom column/post patch. Dimensions must match the header;
 * transparent pixels become palette index zero. Truncated/invalid records fail.
 * This works from raw octets and never casts serialized bytes to C structs. */
int wad_decode_patch(const WadOctet *bytes, uint32_t length,
                     WadOctet *indexed, uint16_t width, uint16_t height);
uint16_t wad_rgb565(WadOctet red, WadOctet green, WadOctet blue);

#endif
