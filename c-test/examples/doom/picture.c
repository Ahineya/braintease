#include "picture.h"
#include "wad.h"
#include "mem.h"
#include <stdio.h>
#include <graphics.h>

static void set_titlepic_index(uint16_t x, uint16_t y, uint16_t value) {
    uint16_t addr = x * 200 + y;
    poke_word(TITLEPIC_BANK, addr, value);
}

static void read_column(uint16_t x) {
    uint16_t row;
    uint16_t length;
    uint16_t i;
    uint16_t pixel;

    while (1) {
        row = wad_read8();
        if (row == 0xFF) {
            return;
        }

        length = wad_read8();
        wad_skip(1);

        i = 0;
        while (i < length) {
            pixel = wad_read8();
            set_titlepic_index(x, row + i, pixel);
            i = i + 1;
        }

        wad_skip(1);
    }
}

void load_titlepic(uint32_t pic_off, uint16_t width) {
    uint16_t x;
    uint32_t col_off;

    for (x = 0; x < width; x++) {
        wad_seek(pic_off + 8UL + (uint32_t)x * 4UL);
        col_off = wad_read32();
        wad_seek(pic_off + col_off);
        read_column(x);

        if ((x & 31) == 31) {
            putchar('.');
        }
    }
    printf("\n");
}

void load_playpal(uint32_t pal_off) {
    uint16_t i;
    uint16_t r;
    uint16_t g;
    uint16_t b;
    uint16_t color;

    wad_seek(pal_off);

    for (i = 0; i < 256; i++) {
        r = wad_read8();
        g = wad_read8();
        b = wad_read8();
        color = rgb565((unsigned char)r, (unsigned char)g, (unsigned char)b);
        poke_word(PALETTE_BANK, i, color);
    }
}

void resolve_titlepic_colors(void) {
    uint16_t x;
    uint16_t y;
    uint16_t addr;
    uint16_t index;
    uint16_t color;

    for (x = 0; x < TITLEPIC_WIDTH; x++) {
        for (y = 0; y < TITLEPIC_HEIGHT; y++) {
            addr = x * 200 + y;
            index = peek_word(TITLEPIC_BANK, addr);
            color = peek_word(PALETTE_BANK, index);
            poke_word(TITLEPIC_BANK, addr, color);
        }
    }
}

static uint16_t get_titlepic_pixel(uint16_t x, uint16_t y) {
    uint16_t addr = x * 200 + y;
    return peek_word(TITLEPIC_BANK, addr);
}

void blit_titlepic(void) {
    uint16_t y;
    uint16_t x;
    uint16_t color;

    for (y = 0; y < SCREEN_HEIGHT; y++) {
        for (x = 0; x < SCREEN_WIDTH; x++) {
            color = get_titlepic_pixel(x * 2, y * 2);
            set_pixel(x, y, color);
        }
    }
    graphics_flush();
}
