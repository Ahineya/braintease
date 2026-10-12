/* Uses actual C globals, a volatile stack local and MMIO, without libc calls. */
volatile unsigned short value = 6;
int main(void) {
    volatile unsigned short local = 7;
    unsigned short result = value * local;
    unsigned short character = result == 42 ? 'Y' : 'N';
    /* Bank-zero offset zero is the TTY port; use the runtime's MMIO convention,
       rather than dereferencing a C null pointer. */
    __asm__("STORE %0, R0, R0" : : "r"(character));
    return result == 42 ? 0 : 1;
}
