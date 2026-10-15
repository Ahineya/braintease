/* Native-word pairs: shifts, XOR and multiplication in unsigned 32-bit C. */
volatile unsigned long seed = 0x12345678UL;
int main(void) {
    unsigned long value = seed;
    unsigned short sum = 0;
    for (unsigned short i = 0; i < 16; ++i) {
        value = (value << 5) ^ (value >> 3) ^ 0x9e3779b9UL;
        value += (unsigned long)(i * 257U + 3U) * 65537UL;
        sum += (unsigned short)value;
        sum ^= (unsigned short)(value >> 16);
    }
    unsigned short character = sum == 52319U ? 'Y' : 'N';
    __asm__("STORE %0, R0, R0" : : "r"(character));
    return sum == 52319U ? 0 : 1;
}
