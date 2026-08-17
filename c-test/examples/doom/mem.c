#include "mem.h"

void poke_word(unsigned short bank, unsigned short addr, unsigned short value) {
    // A0=bank, A1=addr, A2=value (calling convention)
    asm("STORE A2, A0, A1");
}

unsigned short peek_word(unsigned short bank, unsigned short addr) {
    // A0=bank, A1=addr, Rv0=return
    asm("LOAD Rv0, A0, A1");
}
