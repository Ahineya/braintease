#include "mem.h"

// Cross-bank word poke/peek. Naked `STORE A2, A0, A1` / `LOAD Rv0, A0, A1`
// is unsafe: the prologue may spill argument registers, and a missing
// `return` used to make rcc insert `return 0`, wiping Rv0 after LOAD.
// Extended asm binds operands to real registers.

void poke_word(unsigned short bank, unsigned short addr, unsigned short value) {
    __asm__("STORE %0, %1, %2" : : "r"(value), "r"(bank), "r"(addr));
}

unsigned short peek_word(unsigned short bank, unsigned short addr) {
    unsigned short result;
    __asm__("LOAD %0, %1, %2" : "=r"(result) : "r"(bank), "r"(addr));
    return result;
}
