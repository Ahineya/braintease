// C Doom TITLEPIC demo — parity with bfm-doom.bfm.
//
// wad.c, mem.c, picture.c, and this file are compiled as separate
// translation units and linked with the C runtime.
// TITLEPIC pixels live in bank 3, PLAYPAL RGB565 in bank 4 (heap is bank 5+).

#include <stdio.h>
#include <stdint.h>
#include <graphics.h>
#include "wad.h"
#include "mem.h"
#include "picture.h"

int main() {
    uint32_t i;
    uint32_t lump_count;
    uint32_t dir;
    uint32_t filepos;
    uint32_t size;
    uint32_t pic;
    uint32_t pic_size;
    uint32_t pal;
    uint16_t found_pic;
    uint16_t found_pal;
    uint16_t width;
    uint16_t height;
    uint16_t col0;
    char name[8];
    int k;

    wad_seek(0);
    for (k = 0; k < 4; k++) {
        name[k] = (char)wad_read8();
    }
    printf("%c%c%c%c\n", name[0], name[1], name[2], name[3]);

    wad_seek(4);
    lump_count = wad_read32();
    printf("Lump count: %lu\n", lump_count);

    wad_seek(8);
    dir = wad_read32();
    printf("Directory offset: 0x%lx\n", dir);

    found_pic = 0;
    found_pal = 0;
    pic = 0;
    pic_size = 0;
    pal = 0;

    for (i = 0; i < lump_count; i++) {
        wad_seek(dir + i * 16UL);
        filepos = wad_read32();
        size = wad_read32();
        wad_read_name(name);

        if (wad_name_eq(name, "TITLEPIC")) {
            found_pic = 1;
            pic = filepos;
            pic_size = size;
            printf("Found TITLEPIC\n");
            printf("TITLEPIC address: 0x%lx\n", pic);
            printf("TITLEPIC size: %lu\n", pic_size);
        }

        if (wad_name_eq(name, "PLAYPAL")) {
            found_pal = 1;
            pal = filepos;
            printf("Found PLAYPAL\n");
            printf("0x%lx\n", pal);
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

    wad_seek(pic);
    width = wad_read16();
    height = wad_read16();
    printf("TITLEPIC dimensions: %ux%u\n", width, height);

    wad_seek(pic + 8);
    col0 = wad_read16();
    printf("First column offset: %u\n", col0);

    if (lump_count == 1264UL && dir == 0x003fb7b4UL &&
        width == 320 && height == 200 && col0 == 1288) {
        printf("Y\n");
    } else {
        printf("N\n");
    }

    printf("Loading TITLEPIC\n");
    load_titlepic(pic, width);
    printf("pix0: %x\n", peek_word(TITLEPIC_BANK, 0));
    printf("Loading PLAYPAL\n");
    load_playpal(pal);
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
