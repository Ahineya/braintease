void putchar(int c);

typedef union {
    int i;
    char c[4];
} MultiType;

int main() {
    MultiType m2 = (MultiType){.c = {'A', 'B', 'C', 'D'}};
    if (m2.c[0] == 'A') putchar('A'); else putchar('a');
    if (m2.c[1] == 'B') putchar('B'); else putchar('b');
    if (m2.c[2] == 'C') putchar('C'); else putchar('c');
    if (m2.c[3] == 'D') putchar('D'); else putchar('d');
    if (m2.c[0] == 'A' && m2.c[1] == 'B' && m2.c[2] == 'C' && m2.c[3] == 'D') {
        putchar('Y');
    } else {
        putchar('N');
    }
    putchar('\n');
    return 0;
}
