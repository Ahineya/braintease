#include "wad_io.h"
#include "doom_build_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef RIPPLE
#include <mmio.h>
#include <vm_devices.h>
#endif

#ifndef DOOM_WAD_LENGTH
#error Generate DOOM_WAD_LENGTH from the selected WAD fixture
#endif

static void metric(const char *name, uint32_t value) {
    printf("ASSET %s %lu\n", name, (unsigned long)value);
}

static WadOctet *load_lump(WadFile *wad, const WadLump *lump) {
    WadOctet *bytes = malloc((size_t)lump->length);
    if (!bytes) return NULL;
    if (!wad_read_lump(wad, lump, 0UL, bytes, lump->length)) {
        free(bytes);
        return NULL;
    }
    return bytes;
}

static int malformed_checks(WadFile *wad) {
    /* A complete one-column, one-pixel patch, followed by mutations. */
    WadOctet patch[18] = {1,0,1,0,0,0,0,0,12,0,0,0,0,1,0,7,0,255};
    WadOctet pixel[1];
    WadOctet byte;
    uint16_t short_value;
    uint32_t long_value;
    if (wad_range(UINT32_MAX, UINT32_MAX - 1UL, 4UL)) return 0;
    if (!wad_range(UINT32_MAX, UINT32_MAX, 0UL)) return 0;
    if (wad_u16(patch, 1UL, 0UL, &short_value)) return 0;
    if (wad_u32(patch, 18UL, UINT32_MAX, &long_value)) return 0;
    if (wad_read(wad, wad->length, &byte, 1UL)) return 0;
    if (!wad_decode_patch(patch, 18UL, pixel, 1, 1) || pixel[0] != 7) return 0;
    if (wad_decode_patch(patch, 17UL, pixel, 1, 1)) return 0;
    patch[8] = 8; /* Column points into its own offset table. */
    if (wad_decode_patch(patch, 18UL, pixel, 1, 1)) return 0;
    patch[8] = 12;
    patch[12] = 1; /* Post runs below image height. */
    if (wad_decode_patch(patch, 18UL, pixel, 1, 1)) return 0;
    patch[12] = 0;
    patch[13] = 255; /* Truncated post payload. */
    if (wad_decode_patch(patch, 18UL, pixel, 1, 1)) return 0;
    metric("malformed_checks", 10UL);
    return 1;
}

static int texture_checks(WadFile *wad) {
    WadLump lump;
    WadOctet *bytes;
    uint32_t names;
    uint32_t textures;
    uint32_t i;
    uint32_t references = 0;
    if (!wad_find(wad, "PNAMES", &lump)) return 0;
    bytes = load_lump(wad, &lump);
    if (!bytes) return 0;
    if (!wad_u32(bytes, lump.length, 0UL, &names) ||
        names > (lump.length - 4UL) / 8UL) { free(bytes); return 0; }
    free(bytes);
    metric("pnames", names);
    if (!wad_find(wad, "TEXTURE1", &lump)) return 0;
    bytes = load_lump(wad, &lump);
    if (!bytes) return 0;
    if (!wad_u32(bytes, lump.length, 0UL, &textures) ||
        textures > (lump.length - 4UL) / 4UL) { free(bytes); return 0; }
    for (i = 0; i < textures; ++i) {
        uint32_t offset;
        uint16_t width;
        uint16_t height;
        uint16_t patches;
        uint16_t j;
        if (!wad_u32(bytes, lump.length, 4UL + i * 4UL, &offset) ||
            offset < 4UL + textures * 4UL || !wad_range(lump.length, offset, 22UL)) {
            free(bytes); return 0;
        }
        wad_u16(bytes, lump.length, offset + 12UL, &width);
        wad_u16(bytes, lump.length, offset + 14UL, &height);
        wad_u16(bytes, lump.length, offset + 20UL, &patches);
        if (!width || !height || !wad_range(lump.length, offset + 22UL, (uint32_t)patches * 10UL)) {
            free(bytes); return 0;
        }
        for (j = 0; j < patches; ++j) {
            uint16_t patch;
            wad_u16(bytes, lump.length, offset + 26UL + (uint32_t)j * 10UL, &patch);
            if ((uint32_t)patch >= names) { free(bytes); return 0; }
            ++references;
        }
    }
    free(bytes);
    metric("texture1", textures);
    metric("texture_patch_references", references);
    return 1;
}

static int map_checks(WadFile *wad) {
    static const char *names[10] = {
        "THINGS", "LINEDEFS", "SIDEDEFS", "VERTEXES", "SEGS",
        "SSECTORS", "NODES", "SECTORS", "REJECT", "BLOCKMAP"
    };
    static const uint16_t sizes[10] = {10,14,30,4,12,4,28,26,0,2};
    static const char *metrics[10] = {
        "e1m1_things", "e1m1_linedefs", "e1m1_sidedefs", "e1m1_vertexes", "e1m1_segs",
        "e1m1_subsectors", "e1m1_nodes", "e1m1_sectors", "e1m1_reject_octets", "e1m1_blockmap_words"
    };
    WadLump lumps[10];
    uint32_t counts[10];
    uint32_t marker = wad->lump_count;
    uint32_t n;
    uint16_t i;
    int player_found = 0;
    for (n = 0; n < wad->lump_count; ++n) {
        WadLump entry;
        wad_lump_at(wad, n, &entry);
        if (strcmp(entry.name, "E1M1") == 0) marker = n;
    }
    if (marker >= wad->lump_count || wad->lump_count - marker <= 10UL) return 0;
    for (i = 0; i < 10; ++i) {
        if (!wad_lump_at(wad, marker + 1UL + i, &lumps[i]) || strcmp(lumps[i].name, names[i]) != 0)
            return 0;
        if (sizes[i] && lumps[i].length % sizes[i]) return 0;
        counts[i] = sizes[i] ? lumps[i].length / sizes[i] : lumps[i].length;
        metric(metrics[i], counts[i]);
    }
    if (!counts[0] || !counts[1] || !counts[2] || !counts[3] || !counts[4] ||
        !counts[5] || !counts[7] || lumps[9].length < 8UL) return 0;
    if (counts[7] > 65535UL || (counts[7] * counts[7] + 7UL) / 8UL > lumps[8].length) return 0;
    for (i = 0; i < 8; ++i) {
        WadOctet *bytes = load_lump(wad, &lumps[i]);
        if (!bytes) return 0;
        for (n = 0; n < counts[i]; ++n) {
            uint32_t offset = n * sizes[i];
            uint16_t a;
            uint16_t b;
            uint16_t c;
            uint16_t d;
            if (i == 0) {
                wad_u16(bytes, lumps[i].length, offset + 6UL, &a);
                if (a == 1U && !player_found) {
                    wad_u16(bytes, lumps[i].length, offset, &a);
                    wad_u16(bytes, lumps[i].length, offset + 2UL, &b);
                    wad_u16(bytes, lumps[i].length, offset + 4UL, &c);
                    printf("ASSET e1m1_player_x %ld\n", (long)(int16_t)a);
                    printf("ASSET e1m1_player_y %ld\n", (long)(int16_t)b);
                    metric("e1m1_player_angle", c);
                    player_found = 1;
                }
            } else if (i == 1) {
                wad_u16(bytes, lumps[i].length, offset, &a);
                wad_u16(bytes, lumps[i].length, offset + 2UL, &b);
                wad_u16(bytes, lumps[i].length, offset + 10UL, &c);
                wad_u16(bytes, lumps[i].length, offset + 12UL, &d);
                if (a >= counts[3] || b >= counts[3] ||
                    (c != 65535U && c >= counts[2]) || (d != 65535U && d >= counts[2])) {
                    free(bytes); return 0;
                }
            } else if (i == 2) {
                wad_u16(bytes, lumps[i].length, offset + 28UL, &a);
                if (a >= counts[7]) { free(bytes); return 0; }
            } else if (i == 4) {
                wad_u16(bytes, lumps[i].length, offset, &a);
                wad_u16(bytes, lumps[i].length, offset + 2UL, &b);
                wad_u16(bytes, lumps[i].length, offset + 6UL, &c);
                wad_u16(bytes, lumps[i].length, offset + 8UL, &d);
                if (a >= counts[3] || b >= counts[3] || c >= counts[1] || d > 1U) {
                    free(bytes); return 0;
                }
            } else if (i == 5) {
                wad_u16(bytes, lumps[i].length, offset, &a);
                wad_u16(bytes, lumps[i].length, offset + 2UL, &b);
                if ((uint32_t)b + a > counts[4]) { free(bytes); return 0; }
            } else if (i == 6) {
                uint16_t child;
                for (child = 0; child < 2; ++child) {
                    wad_u16(bytes, lumps[i].length, offset + 24UL + (uint32_t)child * 2UL, &a);
                    if ((a & 32768U) ? ((a & 32767U) >= counts[5]) : (a >= counts[6])) {
                        free(bytes); return 0;
                    }
                }
            }
        }
        free(bytes);
    }
    return player_found;
}

static int title_frame(WadFile *wad, const char *output) {
    WadLump title;
    WadLump palette;
    WadOctet colors[768];
    WadOctet *patch;
    WadOctet *indexed;
    unsigned short *frame;
    uint32_t i;
    if (!wad_find(wad, "TITLEPIC", &title) || !wad_find(wad, "PLAYPAL", &palette)) return 0;
    if (!wad_read_lump(wad, &palette, 0UL, colors, 768UL)) return 0;
    patch = load_lump(wad, &title);
    indexed = malloc(64000UL * sizeof(WadOctet));
    frame = malloc(64000UL * sizeof(unsigned short));
    if (!patch || !indexed || !frame) { free(patch); free(indexed); free(frame); return 0; }
    if (!wad_decode_patch(patch, title.length, indexed, 320, 200)) {
        free(patch); free(indexed); free(frame); return 0;
    }
    for (i = 0; i < 64000UL; ++i) {
        uint16_t color = (uint16_t)indexed[i] * 3U;
        frame[i] = (unsigned short)wad_rgb565(colors[color], colors[color + 1], colors[color + 2]);
    }
    metric("title_width", 320UL);
    metric("title_height", 200UL);
#ifdef RIPPLE
    display_set_mode(DISP_MODE_RGB565_EXT);
    if (vm_present_rgb565(frame, 320, 200, 320) != VM_DEVICE_OK) {
        free(patch); free(indexed); free(frame); return 0;
    }
#else
    {
        FILE *file = fopen(output, "wb");
        if (!file) { free(patch); free(indexed); free(frame); return 0; }
        for (i = 0; i < 64000UL; ++i) {
            if (fputc(frame[i] & 255U, file) == EOF || fputc(frame[i] >> 8, file) == EOF) {
                fclose(file); free(patch); free(indexed); free(frame); return 0;
            }
        }
        if (fclose(file) != 0) { free(patch); free(indexed); free(frame); return 0; }
    }
#endif
    free(patch);
    free(indexed);
    free(frame);
    return 1;
}

#ifdef RIPPLE
int main(void) {
    const char *path = "doom1.wad";
    const char *output = NULL;
#else
int main(int argc, char **argv) {
    const char *path;
    const char *output;
    if (argc != 3) { fprintf(stderr, "usage: assets WAD RAW_RGB565_OUTPUT\n"); return 2; }
    path = argv[1];
    output = argv[2];
#endif
    WadFile wad;
    if (!wad_open(&wad, path, DOOM_WAD_LENGTH)) {
        printf("FAIL wad_open: %s\n", wad.error);
        return 1;
    }
    metric("wad_length", wad.length);
    metric("wad_lumps", wad.lump_count);
    if (!malformed_checks(&wad)) { puts("FAIL malformed checks"); wad_close(&wad); return 3; }
    if (!texture_checks(&wad)) { puts("FAIL texture checks"); wad_close(&wad); return 4; }
    if (!map_checks(&wad)) { puts("FAIL map checks"); wad_close(&wad); return 5; }
    /* All diagnostic output precedes PRESENT: capture may stop at that boundary. */
    puts("ASSET validation_ok 1");
    if (!title_frame(&wad, output)) { puts("FAIL title frame"); wad_close(&wad); return 6; }
    wad_close(&wad);
    return 0;
}
