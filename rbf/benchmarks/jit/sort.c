/* Branches plus overlapping array moves, with a checked weighted checksum. */
volatile unsigned short keys[24];
int main(void) {
    for (int i = 0; i < 24; ++i) keys[i] = (i * 37 + 11) & 127;
    for (int i = 1; i < 24; ++i) {
        unsigned short key = keys[i];
        int j = i - 1;
        while (j >= 0 && keys[j] > key) {
            keys[j + 1] = keys[j];
            --j;
        }
        keys[j + 1] = key;
    }
    unsigned short sum = 0;
    int ordered = 1;
    for (int i = 0; i < 24; ++i) {
        sum += keys[i] * (i + 1);
        if (i && keys[i - 1] > keys[i]) ordered = 0;
    }
    int ok = ordered && sum == 25056U;
    unsigned short character = ok ? 'Y' : 'N';
    __asm__("STORE %0, R0, R0" : : "r"(character));
    return ok ? 0 : 1;
}
