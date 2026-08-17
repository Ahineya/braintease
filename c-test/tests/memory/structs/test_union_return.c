void putchar(int c);

typedef union {
    int i;
    char c[4];
} MultiType;

MultiType make_chars(void) {
    MultiType m = (MultiType){.c = {'A', 'B', 'C', 'D'}};
    return m;
}

int main() {
    MultiType m = make_chars();
    if (m.c[0] == 'A' && m.c[1] == 'B' && m.c[2] == 'C' && m.c[3] == 'D') {
        putchar('Y');
    } else {
        putchar('N');
    }
    putchar('\n');
    return 0;
}
