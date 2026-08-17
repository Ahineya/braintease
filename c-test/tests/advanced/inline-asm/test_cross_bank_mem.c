// Cross-bank STORE/LOAD via extended inline asm (TITLEPIC lives in bank 3).
// mmio_write is bank 0 only; this is the pattern used to poke other banks.

void putchar(int c);

void poke_word(unsigned short bank, unsigned short addr, unsigned short value) {
    __asm__("STORE %0, %1, %2" : : "r"(value), "r"(bank), "r"(addr));
}

unsigned short peek_word(unsigned short bank, unsigned short addr) {
    unsigned short result;
    __asm__("LOAD %0, %1, %2" : "=r"(result) : "r"(bank), "r"(addr));
    return result;
}

int main() {
    unsigned short bank = 3;
    unsigned short got;

    poke_word(bank, 0, 0x1234);
    poke_word(bank, 1, 0xABCD);
    poke_word(bank, 200, 42);

    got = peek_word(bank, 0);
    if (got == 0x1234) {
        putchar('Y');
    } else {
        putchar('N');
    }

    got = peek_word(bank, 1);
    if (got == 0xABCD) {
        putchar('Y');
    } else {
        putchar('N');
    }

    got = peek_word(bank, 200);
    if (got == 42) {
        putchar('Y');
    } else {
        putchar('N');
    }

    putchar('\n');
    return 0;
}
