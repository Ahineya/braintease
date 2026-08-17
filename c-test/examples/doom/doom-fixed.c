// C Doom TITLEPIC demo — parity with bfm-doom.bfm.
//
// Sources are split (wad.c, mem.c, picture.c, this file) but compiled as
// one translation unit: rcc assigns each .c file's globals from address 0,
// so separately compiled objects would overlap in GP.
//
// Storage is 64KB blocks: file offset 0xHHHHhhhh = block 0xHHHH, byte 0xhhhh.
// storage_read_at() returns one byte in the low 8 bits.
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
    putchar(read8());
    putchar(read8());
    putchar(read8());
    putchar(read8());
    br();

    seek_to(0, 4);
    lump_count = read16();
    lump_count_hi = read16();
    puts("Lump count:");
    print_uint(lump_count);
    br();

    seek_to(0, 8);
    dir_lo = read16();
    dir_hi = read16();
    puts("Directory offset:");
    print_hex32(dir_hi, dir_lo);
    br();

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
            puts("Found TITLEPIC");
            puts("TITLEPIC address:");
            print_hex32(pic_hi, pic_lo);
            br();
            puts("TITLEPIC size:");
            print_hex32(pic_size_hi, pic_size_lo);
            br();
        }

        if (names_equal(name, "PLAYPAL")) {
            found_pal = 1;
            pal_lo = file_lo;
            pal_hi = file_hi;
            puts("Found PLAYPAL");
            print_hex32(pal_hi, pal_lo);
            br();
        }

        if (found_pic && found_pal) {
            break;
        }
    }

    if (!found_pic) {
        puts("TITLEPIC not found");
        return 1;
    }
    if (!found_pal) {
        puts("PLAYPAL not found");
        return 1;
    }

    seek_to(pic_hi, pic_lo);
    width = read16();
    height = read16();
    puts("TITLEPIC dimensions:");
    print_uint(width);
    putchar('x');
    print_uint(height);
    br();

    seek_to(pic_hi, pic_lo);
    skip(8);
    col0 = read16();
    puts("First column offset:");
    print_uint(col0);
    br();

    if (lump_count_hi == 0 && lump_count == 1264 &&
        dir_hi == 0x003f && dir_lo == 0xb7b4 &&
        width == 320 && height == 200 && col0 == 1288) {
        putchar('Y');
    } else {
        putchar('N');
    }
    br();

    puts("Loading TITLEPIC");
    load_titlepic(pic_hi, pic_lo, width);
    puts("pix0:");
    print_hex16(peek_word(TITLEPIC_BANK, 0));
    br();
    puts("Loading PLAYPAL");
    load_playpal(pal_hi, pal_lo);
    puts("pal0:");
    print_hex16(peek_word(PALETTE_BANK, 0));
    br();
    puts("Resolving colors");
    resolve_titlepic_colors();
    puts("pix0r:");
    print_hex16(peek_word(TITLEPIC_BANK, 0));
    br();
    puts("Drawing");

    graphics_init(SCREEN_WIDTH, SCREEN_HEIGHT);

    while (1) {
        blit_titlepic();
    }

    return 0;
}
