// Patch end-of-column marker is byte 0xFF compared as unsigned short.

void putchar(int c);

int main() {
    unsigned short row;

    row = 0xFF;
    if (row == 0xFF) {
        putchar('Y');
    } else {
        putchar('N');
    }

    row = 255;
    if (row == 0xFF) {
        putchar('Y');
    } else {
        putchar('N');
    }

    row = 0;
    if (row != 0xFF) {
        putchar('Y');
    } else {
        putchar('N');
    }

    putchar('\n');
    return 0;
}
