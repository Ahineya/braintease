// Naked asm that assumes A0/A1/A2 still hold parameters after the prologue.
// Doom's first poke/peek used this __char_patch.c style and hung the column walk.

void putchar(int c);

void poke_naked(unsigned short bank, unsigned short addr, unsigned short value) {
    asm("STORE A2, A0, A1");
}

unsigned short peek_word(unsigned short bank, unsigned short addr) {
    unsigned short result;
    __asm__("LOAD %0, %1, %2" : "=r"(result) : "r"(bank), "r"(addr));
    return result;
}

int main() {
    unsigned short bank = 3;
    unsigned short got;

    poke_naked(bank, 0, 0x1111);
    poke_naked(bank, 5, 0x2222);

    got = peek_word(bank, 0);
    if (got == 0x1111) {
        putchar('Y');
    } else {
        putchar('N');
    }

    got = peek_word(bank, 5);
    if (got == 0x2222) {
        putchar('Y');
    } else {
        putchar('N');
    }

    putchar('\n');
    return 0;
}
