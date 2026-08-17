// 8-byte WAD lump names vs C string literals (TITLEPIC is 8 chars, PLAYPAL is 7 + NUL).

void putchar(int c);

int names_equal(char *a, char *b) {
    int i;
    for (i = 0; i < 8; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

int main() {
    char title[8];
    char pal[8];
    int k;

    title[0] = 'T';
    title[1] = 'I';
    title[2] = 'T';
    title[3] = 'L';
    title[4] = 'E';
    title[5] = 'P';
    title[6] = 'I';
    title[7] = 'C';

    pal[0] = 'P';
    pal[1] = 'L';
    pal[2] = 'A';
    pal[3] = 'Y';
    pal[4] = 'P';
    pal[5] = 'A';
    pal[6] = 'L';
    pal[7] = 0;

    if (names_equal(title, "TITLEPIC")) {
        putchar('Y');
    } else {
        putchar('N');
    }

    if (names_equal(pal, "PLAYPAL")) {
        putchar('Y');
    } else {
        putchar('N');
    }

    if (!names_equal(title, "PLAYPAL")) {
        putchar('Y');
    } else {
        putchar('N');
    }

    // Index compare without a helper (char-by-char in main)
    k = 0;
    if (title[0] == 'T' && title[7] == 'C') {
        k = 1;
    }
    if (k == 1) {
        putchar('Y');
    } else {
        putchar('N');
    }

    putchar('\n');
    return 0;
}
