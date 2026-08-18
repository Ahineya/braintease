// C Doom TITLEPIC demo — parity with bfm-doom.bfm.
//
// wad.c / mem.c / picture.c are #included and compiled as one translation unit.
// Storage is 64KB blocks: file offset 0xHHHHhhhh = block 0xHHHH, byte 0xhhhh.
// Each storage word holds two little-endian file bytes; wad.c unpacks them.
// int is 16-bit, so 32-bit WAD offsets are (hi, lo) pairs.
// TITLEPIC pixels live in bank 3, PLAYPAL RGB565 in bank 4 (heap is bank 5+).

#include <stdio.h>
#include <graphics.h>
#include "wad.h"
#include "mem.h"
#include "picture.h"

#include "wad.c"
#include "mem.c"
#include "picture.c"

int main() {
    unsigned short i;
    unsigned short lump_count;
    unsigned short lump_count_hi;
    unsigned short dir_hi;
    unsigned short dir_lo;
    unsigned short found_pic;
    unsigned short found_pal;
    unsigned short file_hi;
    unsigned short file_lo;
    unsigned short size_hi;
    unsigned short size_lo;
    unsigned short pic_hi;
    unsigned short pic_lo;
    unsigned short pic_size_hi;
    unsigned short pic_size_lo;
    unsigned short pal_hi;
    unsigned short pal_lo;
    unsigned short width;
    unsigned short height;
    unsigned short col0;
    char name[8];
    int k;

    seek_to(0, 0);
    name[0] = read8();
    name[1] = read8();
    name[2] = read8();
    name[3] = read8();
    printf("%c%c%c%c\n", name[0], name[1], name[2], name[3]);

    seek_to(0, 4);
    lump_count = read16();
    lump_count_hi = read16();
    printf("Lump count: %u\n", lump_count);

    seek_to(0, 8);
    dir_lo = read16();
    dir_hi = read16();
    printf("Directory offset: %x%x\n", dir_hi, dir_lo);

    found_pic = 0;
    found_pal = 0;
    pic_hi = 0;
    pic_lo = 0;
    pic_size_hi = 0;
    pic_size_lo = 0;
    pal_hi = 0;
    pal_lo = 0;

    for (i = 0; i < lump_count; i++) {
        seek_to(dir_hi, dir_lo);
        skip(i * 16);

        file_lo = read16();
        file_hi = read16();
        size_lo = read16();
        size_hi = read16();

        for (k = 0; k < 8; k++) {
            name[k] = read8();
        }

        if (names_equal(name, "TITLEPIC")) {
            found_pic = 1;
            pic_lo = file_lo;
            pic_hi = file_hi;
            pic_size_lo = size_lo;
            pic_size_hi = size_hi;
            printf("Found TITLEPIC\n");
            printf("TITLEPIC address: %x%x\n", pic_hi, pic_lo);
            printf("TITLEPIC size: %x%x\n", pic_size_hi, pic_size_lo);
        }

        if (names_equal(name, "PLAYPAL")) {
            found_pal = 1;
            pal_lo = file_lo;
            pal_hi = file_hi;
            printf("Found PLAYPAL\n");
            printf("%x%x\n", pal_hi, pal_lo);
        }

        if (found_pic && found_pal) {
            break;
        }
    }

    if (!found_pic) {
        printf("TITLEPIC not found\n");
        return 1;
    }
    if (!found_pal) {
        printf("PLAYPAL not found\n");
        return 1;
    }

    seek_to(pic_hi, pic_lo);
    width = read16();
    height = read16();
    printf("TITLEPIC dimensions: %ux%u\n", width, height);

    seek_to(pic_hi, pic_lo);
    skip(8);
    col0 = read16();
    printf("First column offset: %u\n", col0);

    if (lump_count_hi == 0 && lump_count == 1264 &&
        dir_hi == 0x003f && dir_lo == 0xb7b4 &&
        width == 320 && height == 200 && col0 == 1288) {
        printf("Y\n");
    } else {
        printf("N\n");
    }

    printf("Loading TITLEPIC\n");
    load_titlepic(pic_hi, pic_lo, width);
    printf("pix0: %x\n", peek_word(TITLEPIC_BANK, 0));
    printf("Loading PLAYPAL\n");
    load_playpal(pal_hi, pal_lo);
    printf("pal0: %x\n", peek_word(PALETTE_BANK, 0));
    printf("Resolving colors\n");
    resolve_titlepic_colors();
    printf("pix0r: %x\n", peek_word(TITLEPIC_BANK, 0));
    printf("Drawing\n");

    graphics_init(SCREEN_WIDTH, SCREEN_HEIGHT);

    while (1) {
        blit_titlepic();
    }

    return 0;
}
