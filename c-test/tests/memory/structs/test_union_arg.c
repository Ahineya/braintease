void putchar(int c);

typedef union {
    int i;
    char c[4];
} MultiType;

int first_char(MultiType u) {
    return u.c[0];
}

int main() {
    MultiType m = (MultiType){.c = {'A', 'B', 'C', 'D'}};
    if (first_char(m) == 'A') {
        putchar('Y');
    } else {
        putchar('N');
    }
    putchar('\n');
    return 0;
}
