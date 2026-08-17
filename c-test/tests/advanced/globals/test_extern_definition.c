// Including a header with `extern T x;` plus a definition in the same TU
// (unity-build of wad.h + wad.c) must not be a redefinition error.

extern unsigned short cur_hi;
unsigned short cur_hi;

void putchar(int c);

int main() {
    cur_hi = 42;
    if (cur_hi == 42) {
        putchar('Y');
    } else {
        putchar('N');
    }
    putchar('\n');
    return 0;
}
