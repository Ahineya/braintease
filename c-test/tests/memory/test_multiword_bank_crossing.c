#include <stdint.h>

#ifndef BANK_SIZE
#define BANK_SIZE 64000
#endif

static unsigned int peek(unsigned int bank, unsigned int address) {
    unsigned int result;
    __asm__("LOAD %0, %1, %2" : "=r"(result) : "r"(bank), "r"(address));
    return result;
}

static void *at(unsigned int bank, unsigned int address) {
    return (void *)(((uintptr_t)bank << 16) | address);
}

int main(void) {
    unsigned long *two = (unsigned long *)at(8, BANK_SIZE - 1);
    unsigned long long *four = (unsigned long long *)at(16, BANK_SIZE - 2);
    unsigned long **pointer = (unsigned long **)at(24, BANK_SIZE - 1);

    *two = 0x12345678UL;
    if (peek(8, BANK_SIZE - 1) != 0x5678U || peek(9, 0) != 0x1234U ||
        *two != 0x12345678UL) return 1;

    *four = 0x0123456789abcdefULL;
    if (peek(16, BANK_SIZE - 2) != 0xcdefU || peek(16, BANK_SIZE - 1) != 0x89abU ||
        peek(17, 0) != 0x4567U || peek(17, 1) != 0x0123U ||
        *four != 0x0123456789abcdefULL) return 1;

    *pointer = two;
    if (peek(24, BANK_SIZE - 1) != BANK_SIZE - 1 || peek(25, 0) != 8 ||
        *pointer != two || **pointer != 0x12345678UL) return 1;

    __asm__("STORE %0, R0, R0" : : "r"('Y'));
    __asm__("STORE %0, R0, R0" : : "r"('\n'));
    return 0;
}
