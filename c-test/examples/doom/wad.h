#ifndef DOOM_WAD_H
#define DOOM_WAD_H

// Byte-addressed IWAD cursor. Disk blocks are 64KB of file bytes:
// file offset 0xHHHHhhhh = block 0xHHHH, byte 0xhhhh.
// MMIO storage is word-addressed, so read8() maps byte 0xhhhh onto
// word 0xhhhh/2 and takes the low or high byte.
// int is 16-bit, so 32-bit offsets are (hi, lo) pairs.

void seek_to(unsigned short hi, unsigned short lo);
void skip(unsigned short n);
unsigned short read8(void);
unsigned short read16(void);

int names_equal(char *a, char *b);

#endif
