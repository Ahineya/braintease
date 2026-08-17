#ifndef DOOM_MEM_H
#define DOOM_MEM_H

// Cross-bank word poke/peek. C mmio_write is bank 0 only;
// TITLEPIC lives in bank 3 and PLAYPAL in bank 4.

void poke_word(unsigned short bank, unsigned short addr, unsigned short value);
unsigned short peek_word(unsigned short bank, unsigned short addr);

#endif
