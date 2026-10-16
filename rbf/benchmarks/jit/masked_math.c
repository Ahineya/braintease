/* TCP echo's byte-pattern expression: only the low octet of wide arithmetic
 * is observable. Volatile input prevents replacing the work with a constant. */
static volatile unsigned long input = 0x12345678UL;
static unsigned short pattern(unsigned long n) {
    return (n * 37UL + 11UL) % 256UL;
}
int main(void) {
    unsigned short sum = 0;
    for (unsigned short i = 0; i < 32; ++i) {
        sum += pattern(input);
        input = input + 1;
    }
    unsigned short character = sum == 4368U ? 'Y' : 'N';
    __asm__("STORE %0, R0, R0" : : "r"(character));
    return sum == 4368U ? 0 : 1;
}
