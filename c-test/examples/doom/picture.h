#ifndef DOOM_PICTURE_H
#define DOOM_PICTURE_H

#include <stdint.h>

#define TITLEPIC_BANK 3
#define PALETTE_BANK  4
#define TITLEPIC_WIDTH  320
#define TITLEPIC_HEIGHT 200
#define SCREEN_WIDTH  160
#define SCREEN_HEIGHT 100

void load_titlepic(uint32_t pic_off, uint16_t width);
void load_playpal(uint32_t pal_off);
void resolve_titlepic_colors(void);
void blit_titlepic(void);

#endif
