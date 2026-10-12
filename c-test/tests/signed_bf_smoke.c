/* Small standalone regression suitable for the slower Brainfuck VM. */
static void output(int value) {
    __asm__("STORE %0, R0, R0" : : "r"(value));
}

int main(void) {
    int negative = -7;
    int positive = 3;
    unsigned int shift = 1;
    int ok = negative / positive == -2 && negative % positive == -1 &&
        positive / negative == 0 && positive % negative == 3 &&
        negative >> shift == -4;
    output(ok ? 'Y' : 'N');
    output('\n');
    return 0;
}
