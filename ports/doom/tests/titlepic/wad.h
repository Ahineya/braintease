#ifndef DOOM_WAD_H
#define DOOM_WAD_H

#include <stdint.h>

// Byte-addressed IWAD cursor. Disk blocks are 64KB of file bytes:
// file offset 0xHHHHhhhh = block 0xHHHH, byte 0xhhhh.
// MMIO storage is word-addressed, so wad_read8() maps byte 0xhhhh onto
// word 0xhhhh/2 and takes the low or high byte.

void wad_seek(uint32_t offset);
void wad_skip(uint32_t n);
uint16_t wad_read8(void);
uint16_t wad_read16(void);
uint32_t wad_read32(void);
void wad_read_name(char *name);
int wad_name_eq(char *name, char *want);

#endif
