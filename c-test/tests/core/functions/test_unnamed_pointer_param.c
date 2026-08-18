// Unnamed pointer parameter in a prototype — c-testsuite 00025.
void putchar(int c);

int ident(char *);
int bump(char *);

int ident(char *p) {
    return *p;
}

int bump(char *p) {
    return *p + 1;
}

int main() {
    char s = 'A';

    if (ident(&s) == 'A') putchar('Y'); else putchar('N');
    if (bump(&s) == 'B') putchar('Y'); else putchar('N');
    putchar('\n');
    return 0;
}
