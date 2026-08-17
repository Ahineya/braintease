#ifndef DOOM_WAD_H
#define DOOM_WAD_H

// Byte-addressed IWAD cursor. Storage is 64KB blocks:
// file offset 0xHHHHhhhh = block 0xHHHH, byte 0xhhhh.
// int is 16-bit, so 32-bit offsets are (hi, lo) pairs.

void print_digit(int n);
void print_uint(int n);
void print_hex16(unsigned int n);
void print_hex32(unsigned int hi, unsigned int lo);
void br(void);

void seek_to(unsigned short hi, unsigned short lo);
void skip(unsigned short n);
unsigned short read8(void);
unsigned short read16(void);

int names_equal(char *a, char *b);

#endif
