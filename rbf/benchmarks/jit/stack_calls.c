/* Recursion keeps real call frames and volatile stack arrays observable. */
volatile unsigned short seed = 17;
unsigned short fold(unsigned short value, unsigned short depth) {
    volatile unsigned short scratch[4];
    scratch[0] = value;
    scratch[1] = value ^ 0x55aaU;
    scratch[2] = value + depth;
    scratch[3] = value * 3U + 1U;
    if (depth) return fold(scratch[3], depth - 1U) + scratch[1] + scratch[2];
    return scratch[0] ^ scratch[1] ^ scratch[2] ^ scratch[3];
}
int main(void) {
    unsigned short sum = 0;
    for (unsigned short i = 0; i < 16; ++i) sum += fold(seed + i, 4);
    unsigned short character = sum == 62096U ? 'Y' : 'N';
    __asm__("STORE %0, R0, R0" : : "r"(character));
    return sum == 62096U ? 0 : 1;
}
