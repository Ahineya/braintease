/* A single function must span code banks, including an early-return jump
 * beyond the 16-bit object offset range. Keep padding compact in the source. */
#define N1 "NOP\n"
#define N2 N1 N1
#define N4 N2 N2
#define N8 N4 N4
#define N16 N8 N8
#define N32 N16 N16
#define N64 N32 N32
#define N128 N64 N64
#define N256 N128 N128
#define N512 N256 N256
#define N1024 N512 N512
#define N2048 N1024 N1024
#define N4096 N2048 N2048
#define N8192 N4096 N4096
#define N16384 N8192 N8192
#define N32768 N16384 N16384
#define N65536 N32768 N32768

void putchar(int c) { __asm__("STORE A0, R0, R0"); }

int large(int mode) {
    if (mode == 0) return 7;
    __asm__(N65536);
    return mode + 41;
}

int main(void) {
    putchar(large(0) == 7 ? 'Y' : 'N');
    putchar(large(1) == 42 ? 'Y' : 'N');
    putchar('\n');
    return 0;
}
