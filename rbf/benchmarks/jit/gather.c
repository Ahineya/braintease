/* Permuted volatile loads/stores: exercise computed pointers and RAM transfers. */
volatile unsigned short cells[64];
int main(void) {
    unsigned short sum = 0;
    unsigned short i, pass;
    for (i = 0; i < 64; ++i) cells[i] = i * 257U + 123U;
    for (pass = 0; pass < 4; ++pass) {
        for (i = 0; i < 64; ++i) {
            unsigned short at = (i * 13U + pass * 7U) & 63U;
            cells[at] = (cells[at] + (i + 1U) * 17U) ^ (pass * 73U);
            sum += cells[at] ^ i;
        }
    }
    unsigned short character = sum == 25280U ? 'Y' : 'N';
    __asm__("STORE %0, R0, R0" : : "r"(character));
    return sum == 25280U ? 0 : 1;
}
