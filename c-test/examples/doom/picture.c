#include "picture.h"
#include "wad.h"
#include "mem.h"
#include <stdio.h>
#include <graphics.h>

void set_titlepic_index(unsigned short x, unsigned short y, unsigned short value) {
    unsigned short addr = x * 200 + y;
    poke_word(TITLEPIC_BANK, addr, value);
}

void read_column(unsigned short x) {
    unsigned short row;
    unsigned short length;
    unsigned short i;
    unsigned short pixel;

    while (1) {
        row = read8();
        if (row == 0xFF) {
            return;
        }

        length = read8();
        skip(1);

        i = 0;
        while (i < length) {
            pixel = read8();
            set_titlepic_index(x, row + i, pixel);
            i = i + 1;
        }

        skip(1);
    }
}

void load_titlepic(unsigned short pic_hi, unsigned short pic_lo, unsigned short width) {
    unsigned short x;
    unsigned short off_lo;
    unsigned short off_hi;
    unsigned short col_hi;
    unsigned short col_lo;
    unsigned short header_skip;

    x = 0;
    while (x < width) {
        seek_to(pic_hi, pic_lo);
        header_skip = 8 + x * 4;
        skip(header_skip);
        off_lo = read16();
        off_hi = read16();

        col_lo = pic_lo + off_lo;
        if (col_lo < pic_lo) {
            col_hi = pic_hi + off_hi + 1;
        } else {
            col_hi = pic_hi + off_hi;
        }
        seek_to(col_hi, col_lo);
        read_column(x);

        if ((x & 31) == 31) {
            putchar('.');
        }

        x = x + 1;
    }
    br();
}

void load_playpal(unsigned short pal_hi, unsigned short pal_lo) {
    unsigned short i;
    unsigned short r;
    unsigned short g;
    unsigned short b;
    unsigned short color;

    seek_to(pal_hi, pal_lo);

    i = 0;
    while (i < 256) {
        r = read8();
        g = read8();
        b = read8();
        color = rgb565((unsigned char)r, (unsigned char)g, (unsigned char)b);
        poke_word(PALETTE_BANK, i, color);
        i = i + 1;
    }
}

void resolve_titlepic_colors(void) {
    unsigned short x;
    unsigned short y;
    unsigned short addr;
    unsigned short index;
    unsigned short color;

    x = 0;
    while (x < TITLEPIC_WIDTH) {
        y = 0;
        while (y < TITLEPIC_HEIGHT) {
            addr = x * 200 + y;
            index = peek_word(TITLEPIC_BANK, addr);
            color = peek_word(PALETTE_BANK, index);
            poke_word(TITLEPIC_BANK, addr, color);
            y = y + 1;
        }
        x = x + 1;
    }
}

unsigned short get_titlepic_pixel(unsigned short x, unsigned short y) {
    unsigned short addr = x * 200 + y;
    return peek_word(TITLEPIC_BANK, addr);
}

void blit_titlepic(void) {
    unsigned short y;
    unsigned short x;
    unsigned short color;

    y = 0;
    while (y < SCREEN_HEIGHT) {
        x = 0;
        while (x < SCREEN_WIDTH) {
            color = get_titlepic_pixel(x * 2, y * 2);
            set_pixel(x, y, color);
            x = x + 1;
        }
        y = y + 1;
    }
    graphics_flush();
}
