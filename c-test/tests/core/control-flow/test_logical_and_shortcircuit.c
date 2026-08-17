void putchar(int c);

int main() {
    int x = 0;
    if (0 && (x = 1)) {
        putchar('N');
    } else {
        if (x == 0) putchar('Y'); else putchar('N');
    }

    int y = 0;
    if (1 && (y = 1)) {
        if (y == 1) putchar('Y'); else putchar('N');
    } else {
        putchar('N');
    }

    int *p = 0;
    if (p && *p) {
        putchar('N');
    } else {
        putchar('Y');
    }

    putchar('\n');
    return 0;
}
