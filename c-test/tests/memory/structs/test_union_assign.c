void putchar(int c);

typedef union {
    int i;
    char c[4];
} MultiType;

int main() {
    MultiType a = (MultiType){.c = {'A', 'B', 'C', 'D'}};
    MultiType b;
    b = a;
    if (b.c[0] == 'A') putchar('A'); else putchar('a');
    if (b.c[1] == 'B') putchar('B'); else putchar('b');
    if (b.c[2] == 'C') putchar('C'); else putchar('c');
    if (b.c[3] == 'D') putchar('D'); else putchar('d');
    putchar('\n');
    return 0;
}
