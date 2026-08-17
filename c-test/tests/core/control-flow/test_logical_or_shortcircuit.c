void putchar(int c);

int main() {
    int x = 0;
    if (1 || (x = 1)) {
        if (x == 0) putchar('Y'); else putchar('N');
    } else {
        putchar('N');
    }

    int y = 1;
    if (0 || (y = 0)) {
        putchar('N');
    } else {
        if (y == 0) putchar('Y'); else putchar('N');
    }

    int z = 0;
    if (0 || 0) {
        putchar('N');
    } else {
        putchar('Y');
    }

    putchar('\n');
    return 0;
}
