#ifndef DOOM_PICTURE_H
#define DOOM_PICTURE_H

#define TITLEPIC_BANK 3
#define PALETTE_BANK  4
#define TITLEPIC_WIDTH  320
#define TITLEPIC_HEIGHT 200
#define SCREEN_WIDTH  160
#define SCREEN_HEIGHT 100

void load_titlepic(unsigned short pic_hi, unsigned short pic_lo, unsigned short width);
void load_playpal(unsigned short pal_hi, unsigned short pal_lo);
void resolve_titlepic_colors(void);
unsigned short get_titlepic_pixel(unsigned short x, unsigned short y);
void blit_titlepic(void);

#endif
